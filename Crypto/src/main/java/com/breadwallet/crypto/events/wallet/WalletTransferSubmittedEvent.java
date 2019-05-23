package com.breadwallet.crypto.events.wallet;

import com.breadwallet.crypto.Transfer;

public final class WalletTransferSubmittedEvent implements WalletEvent {

    private final Transfer transfer;

    public WalletTransferSubmittedEvent(Transfer transfer) {
        this.transfer = transfer;
    }

    public Transfer getTransfer() {
        return transfer;
    }

    @Override
    public String toString() {
        return "TransferSubmitted";
    }

    @Override
    public <T> T accept(WalletEventVisitor<T> visitor) {
        return visitor.visit(this);
    }
}
