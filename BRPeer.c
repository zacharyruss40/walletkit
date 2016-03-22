//
//  BRPeer.c
//
//  Created by Aaron Voisine on 9/2/15.
//  Copyright (c) 2015 breadwallet LLC.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "BRPeer.h"
#include "BRMerkleBlock.h"
#include "BRAddress.h"
#include "BRSet.h"
#include "BRArray.h"
#include "BRHash.h"
#include "BRInt.h"
#include <stdlib.h>
#include <float.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>	
#include <arpa/inet.h>

#if BITCOIN_TESTNET
#define MAGIC_NUMBER 0x0709110b
#else
#define MAGIC_NUMBER 0xd9b4bef9
#endif
#define HEADER_LENGTH      24
#define MAX_MSG_LENGTH     0x02000000
#define MAX_GETDATA_HASHES 50000
#define ENABLED_SERVICES   0     // we don't provide full blocks to remote nodes
#define PROTOCOL_VERSION   70002
#define MIN_PROTO_VERSION  70002 // peers earlier than this protocol version not supported (need v0.9 txFee relay rules)
#define LOCAL_HOST         ((UInt128) { .u32 = { 0, 0, be32(0xffff), be32(0x7f000001) } })
#define CONNECT_TIMEOUT    3.0

// the standard blockchain download protocol works as follows (for SPV mode):
// - local peer sends getblocks
// - remote peer reponds with inv containing up to 500 block hashes
// - local peer sends getdata with the block hashes
// - remote peer responds with multiple merkleblock and tx messages
// - remote peer sends inv containg 1 hash, of the most recent block
// - local peer sends getdata with the most recent block hash
// - remote peer responds with merkleblock
// - if local peer can't connect the most recent block to the chain (because it started more than 500 blocks behind), go
//   back to first step and repeat until entire chain is downloaded
//
// we modify this sequence to improve sync performance and handle adding bip32 addresses to the bloom filter as needed:
// - local peer sends getheaders
// - remote peer responds with up to 2000 headers
// - local peer immediately sends getheaders again and then processes the headers
// - previous two steps repeat until a header within a week of earliestKeyTime is reached (further headers are ignored)
// - local peer sends getblocks
// - remote peer responds with inv containing up to 500 block hashes
// - local peer sends getdata with the block hashes
// - if there were 500 hashes, local peer sends getblocks again without waiting for remote peer
// - remote peer responds with multiple merkleblock and tx messages, followed by inv containing up to 500 block hashes
// - previous two steps repeat until an inv with fewer than 500 block hashes is received
// - local peer sends just getdata for the final set of fewer than 500 block hashes
// - remote peer responds with multiple merkleblock and tx messages
// - if at any point tx messages consume enough wallet addresses to drop below the bip32 chain gap limit, more addresses
//   are generated and local peer sends filterload with an updated bloom filter
// - after filterload is sent, getdata is sent to re-request recent blocks that may contain new tx matching the filter

typedef enum {
    inv_error = 0,
    inv_tx = 1,
    inv_block = 2,
    inv_merkleblock = 3
} inv_type;

typedef struct {
    BRPeer peer; // superstruct on top of BRPeer
    char host[INET6_ADDRSTRLEN];
    BRPeerStatus status;
    int waitingForNetwork, needsFilterUpdate;
    uint64_t nonce;
    char *useragent;
    uint32_t version, lastblock, earliestKeyTime, currentBlockHeight;
    double startTime, pingTime, disconnectTime;
    int sentVerack, gotVerack, sentGetaddr, sentFilter, sentGetdata, sentMempool, sentGetblocks;
    UInt256 lastBlockHash;
    BRMerkleBlock *currentBlock;
    UInt256 *currentBlockTxHashes, *knownBlockHashes, *knownTxHashes;
    BRSet *knownTxHashSet;
    int socket;
    void *info;
    void (*connected)(void *info);
    void (*disconnected)(void *info, int error);
    void (*relayedPeers)(void *info, const BRPeer peers[], size_t count);
    void (*relayedTx)(void *info, BRTransaction *tx);
    void (*hasTx)(void *info, UInt256 txHash);
    void (*rejectedTx)(void *info, UInt256 txHash, uint8_t code);
    void (*notfound)(void *info, const UInt256 txHashes[], size_t txCount, const UInt256 blockHashes[],
                     size_t blockCount);
    void (*relayedBlock)(void *info, BRMerkleBlock *block);
    BRTransaction *(*requestedTx)(void *info, UInt256 txHash);
    int (*networkIsReachable)(void *info);
    void **pongInfo;
    void (**pongCallback)(void *info, int success);
    pthread_t thread;
} BRPeerContext;

void BRPeerSendVersionMessage(BRPeer *peer);
void BRPeerSendVerackMessage(BRPeer *peer);
void BRPeerSendAddr(BRPeer *peer);

inline static int _BRPeerIsIPv4(BRPeer *peer)
{
    return (peer->address.u64[0] == 0 && peer->address.u32[2] == be32(0xffff));
}

static void _BRPeerDidConnect(BRPeer *peer)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    
    if (ctx->status == BRPeerStatusConnecting && ctx->sentVerack && ctx->gotVerack) {
        peer_log(peer, "handshake completed");
        ctx->disconnectTime = DBL_MAX;
        ctx->status = BRPeerStatusConnected;
        peer_log(peer, "connected with lastblock: %u", ctx->lastblock);
        if (ctx->connected) ctx->connected(ctx->info);
    }
}

static int _BRPeerAcceptVersionMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, strLen = 0, l = 0;
    uint64_t recvServices, fromServices, nonce;
    UInt128 recvAddr, fromAddr;
    uint16_t recvPort, fromPort;
    int r = 1;
    
    if (85 > len) {
        peer_log(peer, "malformed version message, length is %zu, should be >= 85", len);
        r = 0;
    }
    else {
        ctx->version = le32(*(uint32_t *)(msg + off));
        off += sizeof(uint32_t);
    
        if (ctx->version < MIN_PROTO_VERSION) {
            peer_log(peer, "protocol version %u not supported", ctx->version);
            r = 0;
        }
        else {
            peer->services = le64(*(uint64_t *)(msg + off));
            off += sizeof(uint64_t);
            peer->timestamp = le64(*(uint64_t *)(msg + off));
            off += sizeof(uint64_t);
            recvServices = le64(*(uint64_t *)(msg + off));
            off += sizeof(uint64_t);
            recvAddr = *(UInt128 *)(msg + off);
            off += sizeof(UInt128);
            recvPort = be16(*(uint16_t *)(msg + off));
            off += sizeof(uint16_t);
            fromServices = le64(*(uint64_t *)(msg + off));
            off += sizeof(uint64_t);
            fromAddr = *(UInt128 *)(msg + off);
            off += sizeof(UInt128);
            fromPort = be16(*(uint16_t *)(msg + off));
            off += sizeof(uint16_t);
            nonce = le64(*(uint64_t *)(msg + off));
            off += sizeof(uint64_t);
            strLen = BRVarInt(msg + off, (off <= len ? len - off : 0), &l);
            off += l;

            if (off + strLen + sizeof(uint32_t) > len) {
                peer_log(peer, "malformed version message, length is %zu, should be %zu", len,
                         off + strLen + sizeof(uint32_t));
                r = 0;
            }
            else {
                array_clear(ctx->useragent);
                array_add_array(ctx->useragent, msg + off, strLen);
                array_add(ctx->useragent, '\0');
                off += strLen;
                ctx->lastblock = le32(*(uint32_t *)(msg + off));
                off += sizeof(uint32_t);
                peer_log(peer, "got version %u, useragent:\"%s\"", ctx->version, ctx->useragent);
                BRPeerSendVerackMessage(peer);
            }
        }
    }
    
    return r;
}

static int _BRPeerAcceptVerackMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    struct timeval tv;
    int r = 1;
    
    if (ctx->gotVerack) {
        peer_log(peer, "got unexpected verack");
    }
    else {
        gettimeofday(&tv, NULL);
        ctx->pingTime = tv.tv_sec + (double)tv.tv_usec/1000000 - ctx->startTime; // use verack time as initial ping time
        ctx->startTime = 0;
        peer_log(peer, "got verack in %fs", ctx->pingTime);
        ctx->gotVerack = 1;
        _BRPeerDidConnect(peer);
    }
    
    return r;
}

// TODO: relay addresses
static int _BRPeerAcceptAddrMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, count = BRVarInt(msg, len, &off);
    int r = 1;
    
    if (off == 0 || off + count*30 > len) {
        peer_log(peer, "malformed addr message, length is %zu, should be %zu for %zu addresses", len,
                 BRVarIntSize(count) + 30*count, count);
        r = 0;
    }
    else if (count > 1000) {
        peer_log(peer, "dropping addr message, %zu is too many addresses, max is 1000", count);
    }
    else if (ctx->sentGetaddr) { // simple anti-tarpitting tactic, don't accept unsolicited addresses
        BRPeer peers[count], p;
        size_t peersCount = 0;
        time_t now = time(NULL);
        
        peer_log(peer, "got addr with %zu addresses", count);

        for (size_t i = 0; i < count; i++) {
            p.timestamp = le32(*(uint32_t *)(msg + off));
            off += sizeof(uint32_t);
            p.services = le64(*(uint64_t *)(msg + off));
            off += sizeof(uint64_t);
            p.address = *(UInt128 *)(msg + off);
            off += sizeof(UInt128);
            p.port = be16(*(uint16_t *)(msg + off));
            off += sizeof(uint16_t);

            if (! (p.services & SERVICES_NODE_NETWORK)) continue; // skip peers that don't carry full blocks
            if (! _BRPeerIsIPv4(&p)) continue; // ignore IPv6 for now
        
            // if address time is more than 10 min in the future or unknown, set to 5 days old
            if (p.timestamp > now + 10*60 || p.timestamp == 0) p.timestamp = now - 5*24*60*60;
            p.timestamp -= 2*60*60; // subtract two hours
            peers[peersCount++] = p; // add it to the list
        }

        if (peersCount > 0 && ctx->relayedPeers) ctx->relayedPeers(ctx->info, peers, peersCount);
    }

    return r;
}

static int _BRPeerAcceptInvMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, count = BRVarInt(msg, len, &off);
    int r = 1;
    
    if (off == 0 || off + count*36 > len) {
        peer_log(peer, "malformed inv message, length is %zu, should be %zu for %zu items", len,
                 BRVarIntSize(count) + 36*count, count);
        r = 0;
    }
    else if (count > MAX_GETDATA_HASHES) {
        peer_log(peer, "dropping inv message, %zu is too many items, max is %d", count, MAX_GETDATA_HASHES);
    }
    else {
        UInt256 *transactions[count], *blocks[count];
        size_t i, j, k, txCount = 0, blockCount = 0;
        
        peer_log(peer, "got inv with %zu items", count);

        for (i = 0; i < count; i++) {
            switch (le32(*(uint32_t *)(msg + off))) {
                case inv_tx: transactions[txCount++] = (UInt256 *)(msg + off + sizeof(uint32_t)); break;
                case inv_merkleblock: // fall through
                case inv_block: blocks[blockCount++] = (UInt256 *)(msg + off + sizeof(uint32_t)); break;
                default: break;
            }

            off += 36;
        }

        if (txCount > 0 && ! ctx->sentFilter && ! ctx->sentMempool && ! ctx->sentGetblocks) {
            peer_log(peer, "got inv message before laoding a filter");
            r = 0;
        }
        else if (txCount > 10000) { // sanity check
            peer_log(peer, "too many transactions, disconnecting");
            r = 0;
        }
        else if (ctx->currentBlockHeight > 0 && blockCount > 2 && blockCount < 500 &&
                 ctx->currentBlockHeight + array_count(ctx->knownBlockHashes) + blockCount < ctx->lastblock) {
            peer_log(peer, "non-standard inv, %zu is fewer block hashes than expected", blockCount);
            r = 0;
        }

        if (blockCount == 1 && ! UInt256Eq(ctx->lastBlockHash, *blocks[0])) blockCount = 0;
        if (blockCount == 1) ctx->lastBlockHash = *blocks[0];

        UInt256 blockHashes[blockCount], txHashes[txCount], *knownTxHashes = ctx->knownTxHashes;

        for (i = 0; i < blockCount; i++) {
            blockHashes[i] = *blocks[i];
            // remember blockHashes in case we need to re-request them with an updated bloom filter
            array_add(ctx->knownBlockHashes, blockHashes[i]);
        }
        
        while (array_count(ctx->knownBlockHashes) > MAX_GETDATA_HASHES) {
            array_rm_range(ctx->knownBlockHashes, 0, array_count(ctx->knownBlockHashes)/3);
        }
        
        if (ctx->needsFilterUpdate) blockCount = 0;
        
        for (i = 0, j = 0; i < txCount; i++) {
            if (BRSetContains(ctx->knownTxHashSet, transactions[i])) continue; // skip transactions we already have
            txHashes[j++] = *transactions[i];
            array_add(knownTxHashes, txHashes[j - 1]);
            
            if (ctx->knownTxHashes != knownTxHashes) { // check if knownTxHashes was moved to a new memory location
                ctx->knownTxHashes = knownTxHashes;
                BRSetClear(ctx->knownTxHashSet);
                for (k = array_count(knownTxHashes); k > 0; k--) BRSetAdd(ctx->knownTxHashSet, &knownTxHashes[k - 1]);
            }
            else BRSetAdd(ctx->knownTxHashSet, &knownTxHashes[array_count(knownTxHashes) - 1]);
            
            if (ctx->hasTx) ctx->hasTx(ctx->info, txHashes[j - 1]);
        }
        
        txCount = j;
        if (txCount > 0 || blockCount > 0) BRPeerSendGetdata(peer, txHashes, txCount, blockHashes, blockCount);
    
        // to improve chain download performance, if we received 500 block hashes, we request the next 500 block hashes
        if (blockCount >= 500) {
            UInt256 locators[] = { blockHashes[blockCount - 1], blockHashes[0] };
            
            BRPeerSendGetblocks(peer, locators, 2, UINT256_ZERO);
        }
    }
    
    return r;
}

static int _BRPeerAcceptTxMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    BRTransaction *tx = BRTransactionParse(msg, len);
    UInt256 txHash;
    int r = 1;

    if (! tx) {
        peer_log(peer, "malformed tx message with length: %zu", len);
        r = 0;
    }
    else if (! ctx->sentFilter && ! ctx->sentGetdata) {
        peer_log(peer, "got tx message before loading filter");
        BRTransactionFree(tx);
        r = 0;
    }
    else {
        txHash = tx->txHash;
        peer_log(peer, "got tx: %s", uint256_hex_encode(txHash));

        if (ctx->relayedTx) {
            ctx->relayedTx(ctx->info, tx);
        }
        else BRTransactionFree(tx);

        if (ctx->currentBlock) { // we're collecting tx messages for a merkleblock
            for (size_t i = array_count(ctx->currentBlockTxHashes); i > 0; i--) {
                if (! UInt256Eq(txHash, ctx->currentBlockTxHashes[i - 1])) continue;
                array_rm(ctx->currentBlockTxHashes, i - 1);
                break;
            }
        
            if (array_count(ctx->currentBlockTxHashes) == 0) { // we received the entire block including all matched tx
                BRMerkleBlock *block = ctx->currentBlock;
            
                ctx->currentBlock = NULL;
                if (ctx->relayedBlock) ctx->relayedBlock(ctx->info, block);
            }
        }
    }
    
    return r;
}

static int _BRPeerAcceptHeadersMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, count = BRVarInt(msg, len, &off);
    int r = 1;

    if (off == 0 || off + 81*count > len) {
        peer_log(peer, "malformed headers message, length is %zu, should be %zu for %zu items", len,
                 BRVarIntSize(count) + 81*count, count);
        r = 0;
    }
    else {
        peer_log(peer, "got %zu headers", count);
    
        // To improve chain download performance, if this message contains 2000 headers then request the next 2000
        // headers immediately, and switch to requesting blocks when we receive a header newer than earliestKeyTime
        uint32_t timestamp = (count > 0) ? le32(*(uint32_t *)(msg + off + 81*(count - 1) + 68)) : 0;
    
        if (count >= 2000 || (timestamp > 0 && timestamp + 7*24*60*60 + BLOCK_MAX_TIME_DRIFT >= ctx->earliestKeyTime)) {
            size_t last = 0;
            time_t now = time(NULL);
            UInt256 locators[2];
            
            BRSHA256_2(&locators[0], msg + off + 81*(count - 1), 80);
            BRSHA256_2(&locators[1], msg + off, 80);

            if (timestamp > 0 && timestamp + 7*24*60*60 + BLOCK_MAX_TIME_DRIFT >= ctx->earliestKeyTime) {
                // request blocks for the remainder of the chain
                timestamp = (++last < count) ? le32(*(uint32_t *)(msg + off + 81*last + 68)) : 0;

                while (timestamp > 0 && timestamp + 7*24*60*60 + BLOCK_MAX_TIME_DRIFT < ctx->earliestKeyTime) {
                    timestamp = (++last < count) ? le32(*(uint32_t *)(msg + off + 81*last + 68)) : 0;
                }
                
                BRSHA256_2(&locators[0], msg + off + 81*(last - 1), 80);
                BRPeerSendGetblocks(peer, locators, 2, UINT256_ZERO);
            }
            else BRPeerSendGetheaders(peer, locators, 2, UINT256_ZERO);

            for (size_t i = 0; r && i < count; i++) {
                BRMerkleBlock *block = BRMerkleBlockParse(msg + off + 81*i, 81);
                
                if (! BRMerkleBlockIsValid(block, (uint32_t)now)) {
                    peer_log(peer, "invalid block header: %s", uint256_hex_encode(block->blockHash));
                    BRMerkleBlockFree(block);
                    r = 0;
                }
                else if (ctx->relayedBlock) {
                    ctx->relayedBlock(ctx->info, block);
                }
                else BRMerkleBlockFree(block);
            }
        }
        else {
            peer_log(peer, "non-standard headers message, %zu is fewer headers than expected", count);
            r = 0;
        }
    }
    
    return r;
}

static int _BRPeerAcceptGetaddrMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    peer_log(peer, "got getaddr");
    BRPeerSendAddr(peer);
    return 1;
}

static int _BRPeerAcceptGetdataMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, count = BRVarInt(msg, len, &off);
    int r = 1;
    
    if (off == 0 || off + 36*count > len) {
        peer_log(peer, "malformed getdata message, length is %zu, should %zu for %zu items", len,
                 BRVarIntSize(count) + 36*count, count);
        r = 0;
    }
    else if (count > MAX_GETDATA_HASHES) {
        peer_log(peer, "dropping getdata message, %zu is too many items, max is %u", count, MAX_GETDATA_HASHES);
    }
    else {
        const uint8_t *notfound[count];
        size_t notfoundCount = 0;
        BRTransaction *tx = NULL;
        
        peer_log(peer, "got getdata with %zu items", count);
        
        for (size_t i = 0; i < count; i++) {
            inv_type type = le32(*(uint32_t *)(msg + off));
            UInt256 hash = *(UInt256 *)(msg + off + sizeof(uint32_t));
            
            switch (type) {
                case inv_tx:
                    if (ctx->requestedTx) tx = ctx->requestedTx(ctx->info, hash);

                    if (tx) {
                        uint8_t buf[BRTransactionSerialize(tx, NULL, 0)];
                        size_t bufLen = BRTransactionSerialize(tx, buf, sizeof(buf));
                        
                        BRPeerSendMessage(peer, buf, bufLen, MSG_TX);
                        break;
                    }
                    
                    // fall through
                default:
                    notfound[notfoundCount++] = msg + off;
                    break;
            }
            
            off += 36;
        }

        if (notfoundCount > 0) {
            uint8_t buf[BRVarIntSize(notfoundCount) + 36*notfoundCount];
            size_t o = BRVarIntSet(buf, sizeof(buf), notfoundCount);

            for (size_t i = 0; o + 36 <= sizeof(buf) && i < notfoundCount; i++) {
                memcpy(buf + o, notfound[i], 36);
                o += 36;
            }
            
            BRPeerSendMessage(peer, buf, sizeof(buf), MSG_NOTFOUND);
        }
    }

    return r;
}

static int _BRPeerAcceptNotfoundMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, count = BRVarInt(msg, len, &off);
    int r = 1;

    if (off == 0 || off + 36*count > len) {
        peer_log(peer, "malformed notfound message, length is %zu, should be %zu for %zu items", len,
                 BRVarIntSize(count) + 36*count, count);
        r = 0;
    }
    else {
        UInt256 txHashes[count], blockHashes[count];
        size_t txCount = 0, blockCount = 0;
        
        peer_log(peer, "got notfound with %zu items", count);
        
        for (size_t i = 0; i < count; i++) {
            switch (le32(*(uint32_t *)(msg + off))) {
                case inv_tx: txHashes[txCount++] = *(UInt256 *)(msg + off + sizeof(uint32_t)); break;
                case inv_merkleblock: // drop through
                case inv_block: blockHashes[blockCount++] = *(UInt256 *)(msg + off + sizeof(uint32_t)); break;
            }
            
            off += 36;
        }
        
        if (ctx->notfound) ctx->notfound(ctx->info, txHashes, txCount, blockHashes, blockCount);
    }
    
    return r;
}

static int _BRPeerAcceptPingMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    int r = 1;
    
    if (sizeof(uint64_t) > len) {
        peer_log(peer, "malformed ping message, length is %zu, should be %zu", len, sizeof(uint64_t));
        r = 0;
    }
    else {
        peer_log(peer, "got ping");
        BRPeerSendMessage(peer, msg, len, MSG_PONG);
    }

    return r;
}

static int _BRPeerAcceptPongMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    struct timeval tv;
    double pingTime;
    int r = 1;
    
    if (sizeof(uint64_t) > len) {
        peer_log(peer, "malformed pong message, length is %zu, should be %zu", len, sizeof(uint64_t));
        r = 0;
    }
    else if (le64(*(uint64_t *)msg) != ctx->nonce) {
        peer_log(peer, "pong message contained wrong nonce: %llu, expected: %llu", le64(*(uint64_t *)msg), ctx->nonce);
        r = 0;

    }
    else if (array_count(ctx->pongCallback) == 0) {
        peer_log(peer, "got unexpected pong");
        r = 0;
    }
    else {
        if (ctx->startTime > 1) {
            gettimeofday(&tv, NULL);
            pingTime = tv.tv_sec + (double)tv.tv_usec/1000000 - ctx->startTime;

            // 50% low pass filter on current ping time
            ctx->pingTime = ctx->pingTime*0.5 + pingTime*0.5;
            ctx->startTime = 0;
            peer_log(peer, "got pong in %fs", pingTime);
        }
        else peer_log(peer, "got pong");

        if (array_count(ctx->pongCallback) > 0) {
            void (*pongCallback)(void *, int) = ctx->pongCallback[0];
            void *pongInfo = ctx->pongInfo[0];

            array_rm(ctx->pongCallback, 0);
            array_rm(ctx->pongInfo, 0);
            if (pongCallback) pongCallback(pongInfo, 1);
        }
    }
    
    return r;
}

static int _BRPeerAcceptMerkleblockMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    // Bitcoin nodes don't support querying arbitrary transactions, only transactions not yet accepted in a block. After
    // a merkleblock message, the remote node is expected to send tx messages for the tx referenced in the block. When a
    // non-tx message is received we should have all the tx in the merkleblock.
    BRPeerContext *ctx = (BRPeerContext *)peer;
    BRMerkleBlock *block = BRMerkleBlockParse(msg, len);
    int r = 1;
  
    if (! block) {
        peer_log(peer, "malformed merkleblock message with length: %zu", len);
        r = 0;
    }
    else if (! BRMerkleBlockIsValid(block, (uint32_t)time(NULL))) {
        peer_log(peer, "invalid merkleblock: %s", uint256_hex_encode(block->blockHash));
        BRMerkleBlockFree(block);
        r = 0;
    }
    else if (! ctx->sentFilter && ! ctx->sentGetdata) {
        peer_log(peer, "got merkleblock message before loading a filter");
        BRMerkleBlockFree(block);
        r = 0;
    }
    else {
        UInt256 txHashes[BRMerkleBlockTxHashes(block, NULL, 0)];
        size_t count = BRMerkleBlockTxHashes(block, txHashes, sizeof(txHashes)/sizeof(*txHashes));

        for (size_t i = count; i > 0; i--) { // reverse order for more efficient removal as tx arrive
            if (BRSetContains(ctx->knownTxHashSet, &txHashes[i - 1])) continue;
            array_add(ctx->currentBlockTxHashes, txHashes[i - 1]);
        }

        if (array_count(ctx->currentBlockTxHashes) > 0) { // wait til we get all tx messages before processing the block
            ctx->currentBlock = block;
        }
        else if (ctx->relayedBlock) {
            ctx->relayedBlock(ctx->info, block);
        }
        else BRMerkleBlockFree(block);
    }
    
    return r;
}

// described in BIP61: https://github.com/bitcoin/bips/blob/master/bip-0061.mediawiki
static int _BRPeerAcceptRejectMessage(BRPeer *peer, const uint8_t *msg, size_t len)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, strLen = BRVarInt(msg, len, &off);
    int r = 1;
    
    if (off + strLen + sizeof(uint8_t) > len) {
        peer_log(peer, "malformed reject message, length is %zu, should be >= %zu", len,
                 off + strLen + sizeof(uint8_t));
        r = 0;
    }
    else {
        char type[strLen + 1];
        uint8_t code;
        size_t l = 0, hashLen = 0;

        strncpy(type, (const char *)(msg + off), strLen);
        type[strLen] = '\0';
        off += strLen;
        code = msg[off++];
        strLen = BRVarInt(msg + off, (off <= len ? len - off : 0), &l);
        off += l;
        if (strncmp(type, MSG_TX, sizeof(type)) == 0) hashLen = sizeof(UInt256);
        
        if (off + strLen + hashLen > len) {
            peer_log(peer, "malformed reject message, length is %zu, should be >= %zu", len, off + strLen + hashLen);
            r = 0;
        }
        else {
            char reason[strLen + 1];
            UInt256 txHash = UINT256_ZERO;
            
            strncpy(reason, (const char *)(msg + off), strLen);
            reason[strLen] = '\0';
            off += strLen;
            if (hashLen == sizeof(UInt256)) txHash = *(UInt256 *)(msg + off);
            off += hashLen;

            if (! UInt256IsZero(txHash)) {
                peer_log(peer, "rejected %s code: 0x%x reason: \"%s\" txid: %s", type, code, reason,
                         uint256_hex_encode(txHash));
                if (ctx->rejectedTx) ctx->rejectedTx(ctx->info, txHash, code);
            }
            else peer_log(peer, "rejected %s code: 0x%x reason: \"%s\"", type, code, reason);
        }
    }

    return r;
}

static int _BRPeerAcceptMessage(BRPeer *peer, const uint8_t *msg, size_t len, const char *type)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    int r = 1;
    
    if (ctx->currentBlock && strncmp(MSG_TX, type, 12) != 0) { // if we receive a non-tx message, merkleblock is done
        peer_log(peer, "incomplete merkleblock %s, expected %zu more tx, got %s",
                 uint256_hex_encode(ctx->currentBlock->blockHash), array_count(ctx->currentBlockTxHashes), type);
        array_clear(ctx->currentBlockTxHashes);
        ctx->currentBlock = NULL;
        r = 0;
    }
    else if (strncmp(MSG_VERSION, type, 12) == 0) r = _BRPeerAcceptVersionMessage(peer, msg, len);
    else if (strncmp(MSG_VERACK, type, 12) == 0) r = _BRPeerAcceptVerackMessage(peer, msg, len);
    else if (strncmp(MSG_ADDR, type, 12) == 0) r = _BRPeerAcceptAddrMessage(peer, msg, len);
    else if (strncmp(MSG_INV, type, 12) == 0) r = _BRPeerAcceptInvMessage(peer, msg, len);
    else if (strncmp(MSG_TX, type, 12) == 0) r = _BRPeerAcceptTxMessage(peer, msg, len);
    else if (strncmp(MSG_HEADERS, type, 12) == 0) r = _BRPeerAcceptHeadersMessage(peer, msg, len);
    else if (strncmp(MSG_GETADDR, type, 12) == 0) r = _BRPeerAcceptGetaddrMessage(peer, msg, len);
    else if (strncmp(MSG_GETDATA, type, 12) == 0) r = _BRPeerAcceptGetdataMessage(peer, msg, len);
    else if (strncmp(MSG_NOTFOUND, type, 12) == 0) r = _BRPeerAcceptNotfoundMessage(peer, msg, len);
    else if (strncmp(MSG_PING, type, 12) == 0) r = _BRPeerAcceptPingMessage(peer, msg, len);
    else if (strncmp(MSG_PONG, type, 12) == 0) r = _BRPeerAcceptPongMessage(peer, msg, len);
    else if (strncmp(MSG_MERKLEBLOCK, type, 12) == 0) r = _BRPeerAcceptMerkleblockMessage(peer, msg, len);
    else if (strncmp(MSG_REJECT, type, 12) == 0) r = _BRPeerAcceptRejectMessage(peer, msg, len);
    else peer_log(peer, "dropping %s, length %zu, not implemented", type, len);

    return r;
}

static int _BRPeerOpenSocket(BRPeer *peer, double timeout)
{
    struct sockaddr addr;
    struct timeval tv;
    fd_set fds;
    socklen_t addrLen, optLen;
    int socket = ((BRPeerContext *)peer)->socket;
    int count, error = 0, r = 1, arg = fcntl(socket, F_GETFL, NULL);

    if (arg < 0 || fcntl(socket, F_SETFL, arg | O_NONBLOCK) < 0) r = 0; // temporarily set the socket non-blocking
    if (! r) error = errno;

    if (r) {
        memset(&addr, 0, sizeof(addr));
        
        if (_BRPeerIsIPv4(peer)) {
            ((struct sockaddr_in *)&addr)->sin_family = AF_INET;
            ((struct sockaddr_in *)&addr)->sin_addr = *(struct in_addr *)&peer->address.u32[3];
            ((struct sockaddr_in *)&addr)->sin_port = htons(peer->port);
            addrLen = sizeof(struct sockaddr_in);
        }
        else {
            ((struct sockaddr_in6 *)&addr)->sin6_family = AF_INET6;
            ((struct sockaddr_in6 *)&addr)->sin6_addr = *(struct in6_addr *)&peer->address;
            ((struct sockaddr_in6 *)&addr)->sin6_port = htons(peer->port);
            addrLen = sizeof(struct sockaddr_in6);
        }

        if (connect(socket, &addr, addrLen) < 0) error = errno;

        if (error == EINPROGRESS) {
            error = 0;
            optLen = sizeof(error);
            tv.tv_sec = timeout;
            tv.tv_usec = (long)(timeout*1000000) % 1000000;
            FD_ZERO(&fds);
            FD_SET(socket, &fds);
            count = select(socket + 1, NULL, &fds, NULL, &tv);

            if (count <= 0 || getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &optLen) < 0 || error) {
                if (count == 0) error = ETIMEDOUT;
                if (count < 0 || ! error) error = errno;
                r = 0;
            }
        }

        if (r) peer_log(peer, "socket connected");
        fcntl(socket, F_SETFL, arg); // restore socket non-blocking status
    }

    if (! r && error) peer_log(peer, "connect error: %s", strerror(error));
    return r;
}

static void *_peerThreadRoutine(void *arg)
{
    BRPeer *peer = arg;
    BRPeerContext *ctx = arg;
    int error = 0;

    if (_BRPeerOpenSocket(peer, CONNECT_TIMEOUT)) {
        struct timeval tv;
        uint8_t header[HEADER_LENGTH];
        size_t len = 0;
        ssize_t n = 0;

        gettimeofday(&tv, NULL);
        ctx->startTime = tv.tv_sec + (double)tv.tv_usec/1000000;
        BRPeerSendVersionMessage(peer);
        
        while (! error) {
            len = 0;

            while (! error && len < HEADER_LENGTH) {
                n = read(ctx->socket, header + len, sizeof(header) - len);
                if (n >= 0) len += n;
                if (n < 0 && errno != EWOULDBLOCK) error = errno;
                gettimeofday(&tv, NULL);
                if (! error && tv.tv_sec + (double)tv.tv_usec/1000000 >= ctx->disconnectTime) error = ETIMEDOUT;
                
                while (sizeof(uint32_t) <= len && *(uint32_t *)header != le32(MAGIC_NUMBER)) {
                    memmove(header, header + 1, --len); // consume one byte at a time until we find the magic number
                }
            }
            
            if (error) {
                peer_log(peer, "%s", strerror(error));
            }
            else if (header[15] != 0) { // verify header type field is NULL terminated
                peer_log(peer, "malformed message header: type not NULL terminated");
                error = EPROTO;
            }
            else if (len == HEADER_LENGTH) {
                const char *type = (const char *)(header + 4);
                uint32_t msgLen = le32(*(uint32_t *)(header + 16));
                uint32_t checksum = *(uint32_t *)(header + 20);
                
                if (msgLen > MAX_MSG_LENGTH) { // check message length
                    peer_log(peer, "error reading %s, message length %u is too long", type, msgLen);
                    error = EPROTO;
                }
                else {
                    uint8_t payload[msgLen];
                    UInt256 hash;
                    
                    len = 0;
                    
                    while (! error && len < msgLen) {
                        n = read(ctx->socket, payload + len, sizeof(payload) - len);
                        if (n >= 0) len += n;
                        if (n < 0 && errno != EWOULDBLOCK) error = errno;
                        gettimeofday(&tv, NULL);
                        if (! error && tv.tv_sec + (double)tv.tv_usec/1000000 >= ctx->disconnectTime) error = ETIMEDOUT;
                    }
                    
                    if (error) {
                        peer_log(peer, "%s", strerror(error));
                    }
                    else if (len == msgLen) {
                        BRSHA256_2(&hash, payload, msgLen);
                        
                        if (hash.u32[0] != checksum) { // verify checksum
                            peer_log(peer, "error reading %s, invalid checksum %x, expected %x, payload length:%u, "
                                     "SHA256_2:%s", type, be32(hash.u32[0]), be32(checksum), msgLen,
                                     uint256_hex_encode(hash));
                            error = EPROTO;
                        }
                        else if (! _BRPeerAcceptMessage(peer, payload, msgLen, type)) error = EPROTO;
                    }
                }
            }
        }
    }
    
    ctx->status = BRPeerStatusDisconnected;
    close(ctx->socket);
    ctx->socket = -1;
    peer_log(peer, "disconnected");
    
    while (array_count(ctx->pongCallback) > 0) {
        void (*pongCallback)(void *, int) = ctx->pongCallback[0];
        void *pongInfo = ctx->pongInfo[0];
        
        array_rm(ctx->pongCallback, 0);
        array_rm(ctx->pongInfo, 0);
        if (pongCallback) pongCallback(pongInfo, 0);
    }
    
    if (ctx->disconnected) ctx->disconnected(ctx->info, error);
    return NULL; // detached threads don't need to return a value
}

// returns a newly allocated BRPeer struct that must be freed by calling BRPeerFree()
BRPeer *BRPeerNew()
{
    BRPeerContext *ctx = calloc(1, sizeof(BRPeerContext));
    
    array_new(ctx->useragent, 40);
    array_new(ctx->knownBlockHashes, 10);
    array_new(ctx->currentBlockTxHashes, 10);
    array_new(ctx->knownTxHashes, 10);
    ctx->knownTxHashSet = BRSetNew(BRTransactionHash, BRTransactionEq, 10);
    array_new(ctx->pongInfo, 10);
    array_new(ctx->pongCallback, 10);
    ctx->pingTime = DBL_MAX;
    ctx->socket = -1;
    return &ctx->peer;
}

void BRPeerSetCallbacks(BRPeer *peer, void *info,
                        void (*connected)(void *info),
                        void (*disconnected)(void *info, int error),
                        void (*relayedPeers)(void *info, const BRPeer peers[], size_t count),
                        void (*relayedTx)(void *info, BRTransaction *tx),
                        void (*hasTx)(void *info, UInt256 txHash),
                        void (*rejectedTx)(void *info, UInt256 txHash, uint8_t code),
                        void (*relayedBlock)(void *info, BRMerkleBlock *block),
                        void (*notfound)(void *info, const UInt256 txHashes[], size_t txCount,
                                         const UInt256 blockHashes[], size_t blockCount),
                        BRTransaction *(*requestedTx)(void *info, UInt256 txHash),
                        int (*networkIsReachable)(void *info))
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    
    ctx->info = info;
    ctx->connected = connected;
    ctx->disconnected = disconnected;
    ctx->relayedPeers = relayedPeers;
    ctx->relayedTx = relayedTx;
    ctx->hasTx = hasTx;
    ctx->rejectedTx = rejectedTx;
    ctx->relayedBlock = relayedBlock;
    ctx->notfound = notfound;
    ctx->requestedTx = requestedTx;
    ctx->networkIsReachable = networkIsReachable;
}

// set earliestKeyTime to wallet creation time in order to speed up initial sync
void BRPeerSetEarliestKeyTime(BRPeer *peer, uint32_t earliestKeyTime)
{
    ((BRPeerContext *)peer)->earliestKeyTime = earliestKeyTime;
}

// call this when local block height changes (helps detect tarpit nodes)
void BRPeerSetCurrentBlockHeight(BRPeer *peer, uint32_t currentBlockHeight)
{
    ((BRPeerContext *)peer)->currentBlockHeight = currentBlockHeight;
}

// call this to (re)schedule a disconnect in the given number of seconds, or 0 to cancel (useful for sync timeout)
void BRPeerScheduleDisconnect(BRPeer *peer, double seconds)
{
    BRPeerContext *ctx = ((BRPeerContext *)peer);
    struct timeval tv;

    gettimeofday(&tv, NULL);
    ctx->disconnectTime = (seconds < 0) ? DBL_MAX : tv.tv_sec + (double)tv.tv_usec/1000000 + seconds;
}

// current connection status
BRPeerStatus BRPeerConnectStatus(BRPeer *peer)
{
    return ((BRPeerContext *)peer)->status;
}

// open connection to peer and perform handshake
void BRPeerConnect(BRPeer *peer)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    struct timeval tv;
    int on = 1;
    pthread_attr_t attr;

    if (ctx->status == BRPeerStatusDisconnected || ctx->waitingForNetwork) {
        ctx->status = BRPeerStatusConnecting;
    
        if (ctx->networkIsReachable && ! ctx->networkIsReachable(ctx->info)) { // delay until network is reachable
            if (! ctx->waitingForNetwork) peer_log(peer, "waiting for network reachability");
            ctx->waitingForNetwork = 1;
        }
        else {
            peer_log(peer, "connecting");
            ctx->waitingForNetwork = 0;
            gettimeofday(&tv, NULL);
            ctx->disconnectTime = tv.tv_sec + (double)tv.tv_usec/1000000 + CONNECT_TIMEOUT;
            ctx->socket = socket((_BRPeerIsIPv4(peer) ? PF_INET : PF_INET6), SOCK_STREAM, 0);
            
            if (ctx->socket >= 0) {
                tv.tv_sec = 1; // one second timeout for send/receive, so thread doesn't block for too long
                tv.tv_usec = 0;
                setsockopt(ctx->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(ctx->socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                setsockopt(ctx->socket, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef SO_NOSIGPIPE // BSD based systems have a SO_NOSIGPIPE socket option to supress SIGPIPE signals
                setsockopt(ctx->socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            }
            
            if (ctx->socket < 0 || pthread_attr_init(&attr) != 0) {
                peer_log(peer, "error creating socket");
                ctx->status = BRPeerStatusDisconnected;
            }
            else if (pthread_attr_setstacksize(&attr, 512*1024) != 0 || // set stack size (there's no standard)
                     // set thread detached so it'll free resources immediately on exit without waiting for join
                     pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0 ||
                     pthread_create(&ctx->thread, &attr, _peerThreadRoutine, peer) != 0) {
                peer_log(peer, "error creating thread");
                ctx->status = BRPeerStatusDisconnected;
                pthread_attr_destroy(&attr);
            }
        }
    }
}

// close connection to peer
void BRPeerDisconnect(BRPeer *peer)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;

    if (ctx->socket >= 0 && shutdown(ctx->socket, SHUT_RDWR) < 0) peer_log(peer, "%s", strerror(errno));
}

// call this when wallet addresses need to be added to bloom filter
void BRPeerSetNeedsFilterUpdate(BRPeer *peer, int needsFilterUpdate)
{
    ((BRPeerContext *)peer)->needsFilterUpdate = needsFilterUpdate;
}

// display name of peer address
const char *BRPeerHost(BRPeer *peer)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;

    if (ctx->host[0] == '\0') {
        if (_BRPeerIsIPv4(peer)) {
            inet_ntop(AF_INET, &peer->address.u32[3], ctx->host, sizeof(ctx->host));
        }
        else inet_ntop(AF_INET6, &peer->address, ctx->host, sizeof(ctx->host));
    }
    
    return ctx->host;
}

// connected peer version number
uint32_t BRPeerVersion(BRPeer *peer)
{
    return ((BRPeerContext *)peer)->version;
}

// connected peer user agent string
const char *BRPeerUserAgent(BRPeer *peer)
{
    return ((BRPeerContext *)peer)->useragent;
}

// best block height reported by connected peer
uint32_t BRPeerLastBlock(BRPeer *peer)
{
    return ((BRPeerContext *)peer)->lastblock;
}

// average ping time for connected peer
double BRPeerPingTime(BRPeer *peer)
{
    return ((BRPeerContext *)peer)->pingTime;
}

#ifndef MSG_NOSIGNAL   // linux based systems have a MSG_NOSIGNAL send flag, useful for supressing SIGPIPE signals
#define MSG_NOSIGNAL 0 // set to 0 if undefined (BSD has the SO_NOSIGPIPE sockopt, and windows has no signals at all)
#endif

// sends a bitcoin protocol message to peer
void BRPeerSendMessage(BRPeer *peer, const uint8_t *msg, size_t len, const char *type)
{
    if (len > MAX_MSG_LENGTH) {
        peer_log(peer, "failed to send %s, length %zu is too long", type, len);
    }
    else {
        BRPeerContext *ctx = (BRPeerContext *)peer;
        uint8_t buf[HEADER_LENGTH + len], hash[32];
        size_t off = 0;
        ssize_t n = 0;
        struct timeval tv;
        int error = 0;
        
        *(uint32_t *)(buf + off) = le32(MAGIC_NUMBER);
        off += sizeof(uint32_t);
        strncpy((char *)buf + off, type, 12);
        off += 12;
        *(uint32_t *)(buf + off) = le32((uint32_t)len);
        off += sizeof(uint32_t);
        BRSHA256_2(hash, msg, len);
        *(uint32_t *)(buf + off) = *(uint32_t *)hash;
        off += sizeof(uint32_t);
        memcpy(buf + off, msg, len);
        peer_log(peer, "sending %s", type);
        
        if (ctx->socket >= 0) {
            len = 0;
            
            while (! error && len < sizeof(buf)) {
                n = send(ctx->socket, buf + len, sizeof(buf) - len, MSG_NOSIGNAL);
                if (n >= 0) len += n;
                if (n < 0 && errno != EWOULDBLOCK) error = errno;
                gettimeofday(&tv, NULL);
                if (! error && tv.tv_sec + (double)tv.tv_usec/1000000 >= ctx->disconnectTime) error = ETIMEDOUT;
            }
        }
        else error = ENOTCONN;
        
        if (error) {
            peer_log(peer, "%s", strerror(error));
            if (ctx->socket >= 0 && shutdown(ctx->socket, SHUT_RDWR) < 0) peer_log(peer, "%s", strerror(errno));
        }
    }
}

void BRPeerSendVersionMessage(BRPeer *peer)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t off = 0, userAgentLen = strlen(USER_AGENT);
    uint8_t msg[80 + BRVarIntSize(userAgentLen) + userAgentLen + 5];
    
    *(uint32_t *)(msg + off) = le32(PROTOCOL_VERSION); // version
    off += sizeof(uint32_t);
    *(uint64_t *)(msg + off) = le64(ENABLED_SERVICES); // services
    off += sizeof(uint64_t);
    *(uint64_t *)(msg + off) = le64(time(NULL)); // timestamp
    off += sizeof(uint64_t);
    *(uint64_t *)(msg + off) = le64(peer->services); // services of remote peer
    off += sizeof(uint64_t);
    *(UInt128 *)(msg + off) = peer->address; // IPv6 address of remote peer
    off += sizeof(UInt128);
    *(uint16_t *)(msg + off) = be16(peer->port); // port of remote peer
    off += sizeof(uint16_t);
    *(uint64_t *)(msg + off) = le64(ENABLED_SERVICES); // services
    off += sizeof(uint64_t);
    *(UInt128 *)(msg + off) = LOCAL_HOST; // IPv4 mapped IPv6 header
    off += sizeof(UInt128);
    *(uint16_t *)(msg + off) = be16(STANDARD_PORT);
    off += sizeof(uint16_t);
    ctx->nonce = ((uint64_t)BRRand(0) << 32) | (uint64_t)BRRand(0); // random nonce
    *(uint64_t *)(msg + off) = le64(ctx->nonce);
    off += sizeof(uint64_t);
    off += BRVarIntSet(msg + off, sizeof(msg) - off, userAgentLen);
    strncpy((char *)(msg + off), USER_AGENT, userAgentLen); // user agent string
    off += userAgentLen;
    *(uint32_t *)(msg + off) = le32(0); // last block received
    off += sizeof(uint32_t);
    msg[off++] = 0; // relay transactions (no for SPV bloom filter mode)
    BRPeerSendMessage(peer, msg, sizeof(msg), MSG_VERSION);
}

void BRPeerSendVerackMessage(BRPeer *peer)
{
    BRPeerSendMessage(peer, NULL, 0, MSG_VERACK);
    ((BRPeerContext *)peer)->sentVerack = 1;
}

void BRPeerSendAddr(BRPeer *peer)
{
    uint8_t msg[BRVarIntSize(0)];
    size_t len = BRVarIntSet(msg, sizeof(msg), 0);
    
    //TODO: send peer addresses we know about
    BRPeerSendMessage(peer, msg, len, MSG_ADDR);
}

void BRPeerSendFilterload(BRPeer *peer, const uint8_t *filter, size_t len)
{
    ((BRPeerContext *)peer)->sentFilter = 1;
    BRPeerSendMessage(peer, filter, len, MSG_FILTERLOAD);
}

void BRPeerSendMempool(BRPeer *peer)
{
    ((BRPeerContext *)peer)->sentMempool = 1;
    BRPeerSendMessage(peer, NULL, 0, MSG_MEMPOOL);
}

void BRPeerSendGetheaders(BRPeer *peer, const UInt256 locators[], size_t count, UInt256 hashStop)
{
    size_t off = 0, len = sizeof(uint32_t) + BRVarIntSize(count) + sizeof(*locators)*count + sizeof(hashStop);
    uint8_t msg[len];
    
    if (off + sizeof(uint32_t) <= len) *(uint32_t *)(msg + off) = le32(PROTOCOL_VERSION);
    off += sizeof(uint32_t);
    if (off + BRVarIntSize(count) <= len) off += BRVarIntSet(msg + off, len - off, count);

    for (size_t i = 0; i < count; i++) {
        if (off + sizeof(*locators) <= len) *(UInt256 *)(msg + off) = locators[i];
        off += sizeof(*locators);
    }

    if (off + sizeof(hashStop) <= len) *(UInt256 *)(msg + off) = hashStop;
    off += sizeof(hashStop);

    if (off <= len && count > 0) {
        peer_log(peer, "calling getheaders with %zu locators: [%s,%s %s]", count, uint256_hex_encode(locators[0]),
                 (count > 2 ? " ...," : ""), (count > 1 ? uint256_hex_encode(locators[count - 1]) : ""));
        BRPeerSendMessage(peer, msg, off, MSG_GETHEADERS);
    }
}

void BRPeerSendGetblocks(BRPeer *peer, const UInt256 locators[], size_t count, UInt256 hashStop)
{
    size_t off = 0, len = sizeof(uint32_t) + BRVarIntSize(count) + sizeof(*locators)*count + sizeof(hashStop);
    uint8_t msg[len];
    
    if (off + sizeof(uint32_t) <= len) *(uint32_t *)(msg + off) = le32(PROTOCOL_VERSION);
    off += sizeof(uint32_t);
    if (off + BRVarIntSize(count) <= len) off += BRVarIntSet(msg + off, len - off, count);
    
    for (size_t i = 0; i < count; i++) {
        if (off + sizeof(*locators) <= len) *(UInt256 *)(msg + off) = locators[i];
        off += sizeof(*locators);
    }
    
    if (off + sizeof(hashStop) <= len) *(UInt256 *)(msg + off) = hashStop;
    off += sizeof(hashStop);
    
    if (off <= len && count > 0) {
        peer_log(peer, "calling getblocks with %zu locators: [%s,%s %s]", count, uint256_hex_encode(locators[0]),
                 (count > 2 ? " ...," : ""), (count > 1 ? uint256_hex_encode(locators[count - 1]) : ""));
        BRPeerSendMessage(peer, msg, off, MSG_GETBLOCKS);
    }
}

void BRPeerSendInv(BRPeer *peer, const UInt256 txHashes[], size_t count)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    UInt256 *knownTxHashes = ctx->knownTxHashes;
    size_t i, j, hashesCount = array_count(knownTxHashes);

    for (i = 0; i < count; i++) {
        if (! BRSetContains(ctx->knownTxHashSet, &txHashes[i])) {
            array_add(knownTxHashes, txHashes[i]);

            if (ctx->knownTxHashes != knownTxHashes) { // check if knownTxHashes was moved to a new memory location
                ctx->knownTxHashes = knownTxHashes;
                BRSetClear(ctx->knownTxHashSet);
                for (j = array_count(knownTxHashes); j > 0; j--) BRSetAdd(ctx->knownTxHashSet, &knownTxHashes[j - 1]);
            }
            else BRSetAdd(ctx->knownTxHashSet, &knownTxHashes[array_count(knownTxHashes) - 1]);
        }
    }
    
    count = array_count(knownTxHashes) - hashesCount;

    if (count > 0) {
        size_t off = 0, len = BRVarIntSize(count) + (sizeof(uint32_t) + sizeof(*txHashes))*count;
        uint8_t msg[len];
        
        if (off + BRVarIntSize(count) <= len) off += BRVarIntSet(msg + off, len - off, count);
        
        for (i = 0; i < count; i++) {
            if (off + sizeof(uint32_t) <= len) *(uint32_t *)(msg + off) = le32(inv_tx);
            off += sizeof(uint32_t);
            if (off + sizeof(*txHashes) <= len) *(UInt256 *)(msg + off) = knownTxHashes[hashesCount + i];
            off += sizeof(*txHashes);
        }

        if (off <= len) BRPeerSendMessage(peer, msg, off, MSG_INV);
    }
}

void BRPeerSendGetdata(BRPeer *peer, const UInt256 txHashes[], size_t txCount, const UInt256 blockHashes[],
                       size_t blockCount)
{
    size_t off = 0, count = txCount + blockCount;
    
    if (count > MAX_GETDATA_HASHES) { // limit total hash count to MAX_GETDATA_HASHES
        peer_log(peer, "couldn't send getdata, %zu is too many items, max is %d", count, MAX_GETDATA_HASHES);
    }
    else if (count > 0) {
        size_t len = BRVarIntSize(count) + (sizeof(uint32_t) + sizeof(UInt256))*(count);
        uint8_t msg[len];

        if (off + BRVarIntSize(count) <= len) off += BRVarIntSet(msg + off, len - off, count);
        
        for (size_t i = 0; i < txCount; i++) {
            if (off + sizeof(uint32_t) <= len) *(uint32_t *)(msg + off) = le32(inv_tx);
            off += sizeof(uint32_t);
            if (off + sizeof(*txHashes) <= len) *(UInt256 *)(msg + off) = txHashes[i];
            off += sizeof(*txHashes);
        }
        
        for (size_t i = 0; i < blockCount; i++) {
            if (off + sizeof(uint32_t) <= len) *(uint32_t *)(msg + off) = le32(inv_merkleblock);
            off += sizeof(uint32_t);
            if (off + sizeof(*blockHashes) <= len) *(UInt256 *)(msg + off) = blockHashes[i];
            off += sizeof(*blockHashes);
        }
        
        ((BRPeerContext *)peer)->sentGetdata = 1;
        BRPeerSendMessage(peer, msg, off, MSG_GETDATA);
    }
}

void BRPeerSendGetaddr(BRPeer *peer)
{
    ((BRPeerContext *)peer)->sentGetaddr = 1;
    BRPeerSendMessage(peer, NULL, 0, MSG_GETADDR);
}

void BRPeerSendPing(BRPeer *peer, void *info, void (*pongCallback)(void *info, int success))
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    uint64_t msg = le64(ctx->nonce);
    struct timeval tv;
    
    gettimeofday(&tv, NULL);
    ctx->startTime = tv.tv_sec + (double)tv.tv_usec/1000000;
    array_add(ctx->pongInfo, info);
    array_add(ctx->pongCallback, pongCallback);
    BRPeerSendMessage(peer, (uint8_t *)&msg, sizeof(msg), MSG_PING);
}

// useful to get additional tx after a bloom filter update
void BRPeerRerequestBlocks(BRPeer *peer, UInt256 fromBlock)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    size_t i = array_count(ctx->knownBlockHashes);
    
    while (i > 0 && ! UInt256Eq(ctx->knownBlockHashes[i - 1], fromBlock)) i--;
   
    if (i > 0) {
        array_rm_range(ctx->knownBlockHashes, 0, i - 1);
        peer_log(peer, "re-requesting %zu blocks", array_count(ctx->knownBlockHashes));
        BRPeerSendGetdata(peer, NULL, 0, ctx->knownBlockHashes, array_count(ctx->knownBlockHashes));
    }
}

void BRPeerFree(BRPeer *peer)
{
    BRPeerContext *ctx = (BRPeerContext *)peer;
    
    if (ctx->useragent) array_free(ctx->useragent);
    if (ctx->currentBlockTxHashes) array_free(ctx->currentBlockTxHashes);
    if (ctx->knownBlockHashes) array_free(ctx->knownBlockHashes);
    if (ctx->knownTxHashes) array_free(ctx->knownTxHashes);
    if (ctx->knownTxHashSet) BRSetFree(ctx->knownTxHashSet);
    if (ctx->pongInfo) array_free(ctx->pongInfo);
    if (ctx->pongCallback) array_free(ctx->pongCallback);
    free(ctx);
}
