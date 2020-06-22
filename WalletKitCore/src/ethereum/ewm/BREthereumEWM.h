//
//  BREthereumEWM
//  Core Ethereum
//
//  Created by Ed Gamble on 3/5/18.
//  Copyright © 2018-2019 Breadwinner AG.  All rights reserved.
//
//  See the LICENSE file at the project root for license information.
//  See the CONTRIBUTORS file at the project root for a list of contributors.

#ifndef BR_Ethereum_EWM_H
#define BR_Ethereum_EWM_H

#include "ethereum/blockchain/BREthereumNetwork.h"
#include "ethereum/contract/BREthereumContract.h"
#include "BREthereumBase.h"
#include "BREthereumAmount.h"
#include "BREthereumClient.h"

#ifdef __cplusplus
extern "C" {
#endif

/// MARK: - Ethereum Wallet Manager

extern BREthereumEWM
ewmCreate (BREthereumNetwork network,
           BREthereumAccount account,
           BREthereumTimestamp accountTimestamp,
           BRCryptoSyncMode mode,
           BREthereumClient client,
           const char *storagePath,
           uint64_t blockHeight,
           uint64_t confirmationsUntilFinal);

extern BREthereumEWM
ewmCreateWithPaperKey (BREthereumNetwork network,
                       const char *paperKey,
                       BREthereumTimestamp accountTimestamp,
                       BRCryptoSyncMode mode,
                       BREthereumClient client,
                       const char *storagePath,
                       uint64_t blockHeight,
                       uint64_t confirmationsUntilFinal);

extern BREthereumEWM
ewmCreateWithPublicKey (BREthereumNetwork network,
                        BRKey publicKey,
                        BREthereumTimestamp accountTimestamp,
                        BRCryptoSyncMode mode,
                        BREthereumClient client,
                        const char *storagePath,
                        uint64_t blockHeight,
                        uint64_t confirmationsUntilFinal);

extern void
ewmDestroy (BREthereumEWM ewm);

/// MARK: Start Stop

/**
 * Starts the EWM event queue.  Must be called after ewmCreate() and ewmStop()
 *
 * @param ewm
 */
extern void
ewmStart (BREthereumEWM ewm);

/**
 * Stops the EWM event queue (but does not purge existing, queued events).
 *
 * @param ewm 
 */
extern void
ewmStop (BREthereumEWM ewm);

extern BREthereumNetwork
ewmGetNetwork (BREthereumEWM ewm);

extern BREthereumAccount
ewmGetAccount (BREthereumEWM ewm);

/**
 * Get the primary address for `account`.  This is the '0x'-prefixed, 40-char, hex encoded
 * string.  The returned char* is newly allocated, on each call - you MUST free() it.
 */
extern char *
ewmGetAccountPrimaryAddress(BREthereumEWM ewm);

/**
 * Get the public key for `account`.  This is a 0x04-prefixed, 65-byte array.  You own this
 * memory and you MUST free() it.
 */
extern BRKey
ewmGetAccountPrimaryAddressPublicKey(BREthereumEWM ewm);

/**
 * Get the private key for `account`.
 */
extern BRKey
ewmGetAccountPrimaryAddressPrivateKey(BREthereumEWM ewm,
                                      const char *paperKey);

/// MARK: - Connect

extern BREthereumBoolean
ewmConnect(BREthereumEWM ewm);

extern BREthereumBoolean
ewmDisconnect (BREthereumEWM ewm);

extern BREthereumBoolean
ewmIsConnected (BREthereumEWM ewm);

extern BREthereumBoolean
ewmSync (BREthereumEWM ewm,
         BREthereumBoolean pendExistingTransfers);

extern BREthereumBoolean
ewmSyncToDepth (BREthereumEWM ewm,
                BREthereumBoolean pendExistingTransfers,
                BRCryptoSyncDepth depth);

extern BRCryptoSyncMode
ewmGetMode (BREthereumEWM ewm);

extern void
ewmUpdateMode (BREthereumEWM ewm,
               BRCryptoSyncMode mode);

extern uint64_t
ewmGetBlockHeight (BREthereumEWM ewm);

extern void
ewmWipe (BREthereumNetwork network,
         const char *storagePath);

/// MARK: - Wallets

extern BREthereumWallet *
ewmGetWallets (BREthereumEWM ewm);

extern size_t
ewmGetWalletsCount (BREthereumEWM ewm);

extern BREthereumWallet
ewmGetWallet(BREthereumEWM ewm);

extern BREthereumWallet
ewmGetWalletHoldingToken(BREthereumEWM ewm,
                         BREthereumToken token);

/// MARK: - Wallet

extern BREthereumAddress
ewmWalletGetAddress (BREthereumEWM ewm,
                     BREthereumWallet wallet);

extern BREthereumBoolean
ewmWalletHasAddress (BREthereumEWM ewm,
                     BREthereumWallet wallet,
                     BREthereumAddress address);

extern BREthereumToken
ewmWalletGetToken (BREthereumEWM ewm,
                   BREthereumWallet wallet);

extern BREthereumAmount
ewmWalletGetBalance(BREthereumEWM ewm,
                    BREthereumWallet wallet);

extern BREthereumGas
ewmWalletGetGasEstimate(BREthereumEWM ewm,
                        BREthereumWallet wallet,
                        BREthereumTransfer transfer);

extern BREthereumGas
ewmWalletGetDefaultGasLimit(BREthereumEWM ewm,
                            BREthereumWallet wallet);

extern void
ewmWalletSetDefaultGasLimit(BREthereumEWM ewm,
                            BREthereumWallet wallet,
                            BREthereumGas gasLimit);

extern BREthereumGasPrice
ewmWalletGetDefaultGasPrice(BREthereumEWM ewm,
                            BREthereumWallet wallet);

extern void
ewmWalletSetDefaultGasPrice(BREthereumEWM ewm,
                            BREthereumWallet wallet,
                            BREthereumGasPrice gasPrice);

/**
 * Return a newly allocated array of transfers in wallet.  (You own the array and  must call free
 * on it - but not on its elements).
 *
 * @param ewm
 * @param wallet
 *
 * @return array of transfers, will be NULL terminated.
 */
extern BREthereumTransfer *
ewmWalletGetTransfers(BREthereumEWM ewm,
                      BREthereumWallet wallet);

extern int
ewmWalletGetTransferCount(BREthereumEWM ewm,
                          BREthereumWallet wallet);

extern BREthereumTransfer
ewmWalletCreateTransfer(BREthereumEWM ewm,
                        BREthereumWallet wallet,
                        const char *recvAddress,
                        BREthereumAmount amount);

extern BREthereumTransfer
ewmWalletCreateTransferGeneric(BREthereumEWM ewm,
                               BREthereumWallet wallet,
                               const char *recvAddress,
                               BREthereumEther amount,
                               BREthereumGasPrice gasPrice,
                               BREthereumGas gasLimit,
                               const char *data);

extern BREthereumTransfer
ewmWalletCreateTransferWithFeeBasis (BREthereumEWM ewm,
                                     BREthereumWallet wallet,
                                     const char *recvAddress,
                                     BREthereumAmount amount,
                                     BREthereumFeeBasis feeBasis);
extern BREthereumEther
ewmWalletEstimateTransferFee (BREthereumEWM ewm,
                              BREthereumWallet wallet,
                              BREthereumAmount amount,
                              int *overflow);

extern BREthereumEther
ewmWalletEstimateTransferFeeForBasis (BREthereumEWM ewm,
                                      BREthereumWallet wallet,
                                      BREthereumAmount amount,
                                      BREthereumGasPrice price,
                                      BREthereumGas gas,
                                      int *overflow);

extern void
ewmWalletEstimateTransferFeeForTransfer (BREthereumEWM ewm,
                                         BREthereumWallet wallet,
                                         BREthereumCookie cookie,
                                         BREthereumAddress source,
                                         BREthereumAddress target,
                                         BREthereumAmount amount,
                                         BREthereumGasPrice gasPrice,
                                         BREthereumGas gasLimit);

extern void // status, error
ewmWalletSignTransfer(BREthereumEWM ewm,
                      BREthereumWallet wallet,
                      BREthereumTransfer transfer,
                      BRKey privateKey);

extern void // status, error
ewmWalletSignTransferWithPaperKey(BREthereumEWM ewm,
                                  BREthereumWallet wallet,
                                  BREthereumTransfer transfer,
                                  const char *paperKey);

extern void // status, error
ewmWalletSubmitTransfer(BREthereumEWM ewm,
                        BREthereumWallet wid,
                        BREthereumTransfer tid);

extern BREthereumBoolean
ewmWalletCanCancelTransfer (BREthereumEWM ewm,
                            BREthereumWallet wid,
                            BREthereumTransfer tid);

extern BREthereumTransfer // status, error
ewmWalletCreateTransferToCancel(BREthereumEWM ewm,
                                BREthereumWallet wid,
                                BREthereumTransfer tid);

extern BREthereumBoolean
ewmWalletCanReplaceTransfer (BREthereumEWM ewm,
                             BREthereumWallet wid,
                             BREthereumTransfer tid);

extern BREthereumTransfer // status, error
ewmWalletCreateTransferToReplace(BREthereumEWM ewm,
                                 BREthereumWallet wid,
                                 BREthereumTransfer tid,
                                 // ...
                                 BREthereumBoolean updateGasPrice,
                                 BREthereumBoolean updateGasLimit,
                                 BREthereumBoolean updateNonce);

extern uint64_t
ewmWalletGetTransferNonce (BREthereumEWM ewm,
                           BREthereumWallet wallet);

/// MARK: - Transfer

extern void
ewmTransferDelete (BREthereumEWM ewm,
                   BREthereumTransfer transfer);

extern BREthereumAddress
ewmTransferGetTarget (BREthereumEWM ewm,
                      BREthereumTransfer transfer);


extern BREthereumAddress
ewmTransferGetSource (BREthereumEWM ewm,
                      BREthereumTransfer transfer);

extern BREthereumHash
ewmTransferGetIdentifier (BREthereumEWM ewm,
                          BREthereumTransfer transfer);

extern BREthereumHash
ewmTransferGetOriginatingTransactionHash (BREthereumEWM ewm,
                                          BREthereumTransfer transfer);

extern BREthereumAmount
ewmTransferGetAmount(BREthereumEWM ewm,
                     BREthereumTransfer transfer);

extern BREthereumGasPrice
ewmTransferGetGasPrice(BREthereumEWM ewm,
                       BREthereumTransfer transfer,
                       BREthereumEtherUnit unit);

extern BREthereumGas
ewmTransferGetGasLimit(BREthereumEWM ewm,
                       BREthereumTransfer transfer);

extern BREthereumFeeBasis
ewmTransferGetFeeBasis (BREthereumEWM ewm,
                        BREthereumTransfer transfer);

extern uint64_t
ewmTransferGetNonce(BREthereumEWM ewm,
                    BREthereumTransfer transfer);

extern BREthereumBoolean
ewmTransferExtractStatusIncluded (BREthereumEWM ewm,
                                  BREthereumTransfer transfer,
                                  BREthereumHash *blockHash,
                                  uint64_t *blockNumber,
                                  uint64_t *blockTransactionIndex,
                                  uint64_t *blockTimestamp,
                                  BREthereumGas *gasUsed);

extern BREthereumHash
ewmTransferGetBlockHash(BREthereumEWM ewm,
                        BREthereumTransfer transfer);

extern uint64_t
ewmTransferGetBlockNumber(BREthereumEWM ewm,
                          BREthereumTransfer transfer);

extern uint64_t
ewmTransferGetTransactionIndex(BREthereumEWM ewm,
                               BREthereumTransfer transfer);

extern uint64_t
ewmTransferGetBlockTimestamp (BREthereumEWM ewm,
                              BREthereumTransfer transfer);

extern BREthereumGas
ewmTransferGetGasUsed(BREthereumEWM ewm,
                      BREthereumTransfer transfer);

extern uint64_t
ewmTransferGetBlockConfirmations(BREthereumEWM ewm,
                                 BREthereumTransfer transfer);

extern BREthereumTransferStatus
ewmTransferGetStatus (BREthereumEWM ewm,
                      BREthereumTransfer transfer);

extern BREthereumBoolean
ewmTransferIsConfirmed(BREthereumEWM ewm,
                       BREthereumTransfer transfer);

extern BREthereumBoolean
ewmTransferIsSubmitted(BREthereumEWM ewm,
                       BREthereumTransfer transfer);

extern char *
ewmTransferStatusGetError (BREthereumEWM ewm,
                           BREthereumTransfer transfer);

extern int
ewmTransferStatusGetErrorType (BREthereumEWM ewm,
                               BREthereumTransfer transfer);

extern BREthereumBoolean
ewmTransferHoldsToken(BREthereumEWM ewm,
                      BREthereumTransfer transfer,
                      BREthereumToken token);

extern BREthereumToken
ewmTransferGetToken(BREthereumEWM ewm,
                    BREthereumTransfer transfer);

extern BREthereumEther
ewmTransferGetFee(BREthereumEWM ewm,
                  BREthereumTransfer transfer,
                  int *overflow);

/// MARK: - Amount

extern BREthereumAmount
ewmCreateEtherAmountString(BREthereumEWM ewm,
                           const char *number,
                           BREthereumEtherUnit unit,
                           BRCoreParseStatus *status);

extern BREthereumAmount
ewmCreateEtherAmountUnit(BREthereumEWM ewm,
                         uint64_t amountInUnit,
                         BREthereumEtherUnit unit);

extern BREthereumAmount
ewmCreateTokenAmountString(BREthereumEWM ewm,
                           BREthereumToken token,
                           const char *number,
                           BREthereumTokenQuantityUnit unit,
                           BRCoreParseStatus *status);

extern char *
ewmCoerceEtherAmountToString(BREthereumEWM ewm,
                             BREthereumEther ether,
                             BREthereumEtherUnit unit) ;

extern char *
ewmCoerceTokenAmountToString(BREthereumEWM ewm,
                             BREthereumTokenQuantity token,
                             BREthereumTokenQuantityUnit unit);

/// MARK: - Gas Price / Limit

extern BREthereumGasPrice
ewmCreateGasPrice (uint64_t value,
                   BREthereumEtherUnit unit);

extern BREthereumGas
ewmCreateGas (uint64_t value);

/// MARK: - Block Number

extern void
ethereumClientUpdateBlockNumber (BREthereumEWM ewm);

extern BREthereumStatus
ethereumClientAnnounceBlockNumber (BREthereumEWM ewm,
                                   const char *strBlockNumber,
                                   int rid);

/// MARK: Token

extern BREthereumToken
ewmLookupToken (BREthereumEWM ewm,
                BREthereumAddress address);

extern BREthereumToken
ewmCreateToken (BREthereumEWM ewm,
                const char *address,
                const char *symbol,
                const char *name,
                const char *description,
                unsigned int decimals,
                BREthereumGas defaultGasLimit,
                BREthereumGasPrice defaultGasPrice);

///
//extern void // status, error
//ewmWalletSubmitTransferCancel(BREthereumEWM ewm,
//                              BREthereumWalletId wid,
//                              BREthereumTransfer transfer,
//                              const char *paperKey);
//
//extern void // status, error
//ewmWalletSubmitTransferAgain(BREthereumEWM ewm,
//                             BREthereumWalletId wid,
//                             BREthereumTransfer transfer,
//                             const char *paperKey);

//
// Block
//
extern uint64_t
ewmGetBlockHeight(BREthereumEWM ewm);

extern const char *
ewmTransferGetRawDataHexEncoded(BREthereumEWM ewm,
                                BREthereumWallet wallet,
                                BREthereumTransfer transfer,
                                BREthereumBoolean encodeAsSigned,
                                const char *prefix);

extern BRRlpData
ewmTransferGetRLPEncoding (BREthereumEWM ewm,
                           BREthereumWallet wallet,
                           BREthereumTransfer transfer,
                           BREthereumRlpType type,
                           BREthereumBoolean *encoded);

extern BREthereumTransfer
ewmWalletGetTransferByOriginatingTransactionHash (BREthereumEWM ewm,
                                                  BREthereumWallet wallet,
                                                  BREthereumHash hash);


#ifdef __cplusplus
}
#endif

#endif //BR_Ethereum_EWM_H
