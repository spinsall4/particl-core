// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/wallet.h>

#include <amount.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <ui_interface.h>
#include <uint256.h>
#include <util/system.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/ismine.h>
#include <wallet/rpcwallet.h>
#include <wallet/load.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#include <wallet/hdwallet.h>
#include <wallet/rpchdwallet.h>
#include <smsg/smessage.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

void LockWallet(CWallet* pWallet)
{
    LOCK(pWallet->cs_wallet);
    pWallet->nRelockTime = 0;
    pWallet->Lock();
}

namespace interfaces {
namespace {

//! Construct wallet tx struct.
WalletTx MakeWalletTx(interfaces::Chain::Lock& locked_chain, CWallet& wallet, const CWalletTx& wtx)
{
    WalletTx result;
    result.tx = wtx.tx;
    result.txin_is_mine.reserve(wtx.tx->vin.size());
    for (const auto& txin : wtx.tx->vin) {
        result.txin_is_mine.emplace_back(wallet.IsMine(txin));
    }
    if (wtx.tx->IsParticlVersion()) {
        size_t nv = wtx.tx->GetNumVOuts();
        result.txout_is_mine.reserve(nv);
        result.txout_address.reserve(nv);
        result.txout_address_is_mine.reserve(nv);

        for (const auto& txout : wtx.tx->vpout) {
            // Mark data outputs as owned so txn will show as payment to self
            result.txout_is_mine.emplace_back(
                txout->IsStandardOutput() ? wallet.IsMine(txout.get()) : ISMINE_SPENDABLE);
            result.txout_address.emplace_back();

            if (txout->IsStandardOutput()) {
                result.txout_address_is_mine.emplace_back(ExtractDestination(*txout->GetPScriptPubKey(), result.txout_address.back()) ?
                                                          IsMine(wallet, result.txout_address.back()) :
                                                          ISMINE_NO);
            } else {
                result.txout_address_is_mine.emplace_back(ISMINE_NO);
            }
        }
    } else {
        result.txout_is_mine.reserve(wtx.tx->vout.size());
        result.txout_address.reserve(wtx.tx->vout.size());
        result.txout_address_is_mine.reserve(wtx.tx->vout.size());
        for (const auto& txout : wtx.tx->vout) {
            result.txout_is_mine.emplace_back(wallet.IsMine(txout));
            result.txout_address.emplace_back();
            result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ?
                                                          IsMine(wallet, result.txout_address.back()) :
                                                          ISMINE_NO);
        }
    }

    result.credit = wtx.GetCredit(locked_chain, ISMINE_ALL, true);
    result.debit = wtx.GetDebit(ISMINE_ALL);
    result.change = wtx.GetChange();
    result.time = wtx.GetTxTime();
    result.value_map = wtx.mapValue;
    result.is_coinbase = wtx.IsCoinBase();
    result.is_coinstake = wtx.IsCoinStake();
    return result;
}

//! Construct wallet tx struct.
WalletTx MakeWalletTx(CHDWallet& wallet, MapRecords_t::const_iterator irtx)
{
    WalletTx result;
    result.is_record = true;
    result.irtx = irtx;
    result.time = irtx->second.GetTxTime();
    result.partWallet = &wallet;

    result.is_coinbase = false;
    result.is_coinstake = false;

    return result;
}

//! Construct wallet tx status struct.
WalletTxStatus MakeWalletTxStatus(interfaces::Chain::Lock& locked_chain, const CWalletTx& wtx)
{
    WalletTxStatus result;
    result.block_height = locked_chain.getBlockHeight(wtx.m_confirm.hashBlock).get_value_or(std::numeric_limits<int>::max());
    result.blocks_to_maturity = wtx.GetBlocksToMaturity(locked_chain);
    result.depth_in_main_chain = wtx.GetDepthInMainChain(locked_chain);
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.tx->nLockTime;
    result.is_final = locked_chain.checkFinalTx(*wtx.tx);
    result.is_trusted = wtx.IsTrusted(locked_chain);
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_in_main_chain = wtx.IsInMainChain(locked_chain);
    return result;
}

WalletTxStatus MakeWalletTxStatus(interfaces::Chain::Lock& locked_chain, CHDWallet &wallet, const uint256 &hash, const CTransactionRecord &rtx)
{
    LockAssertion lock(::cs_main); // Temporary, for CheckFinalTx below. Removed in upcoming commit.

    WalletTxStatus result;
    result.block_height = locked_chain.getBlockHeight(rtx.blockHash).get_value_or(std::numeric_limits<int>::max());
    result.blocks_to_maturity = 0;
    result.depth_in_main_chain = wallet.GetDepthInMainChain(locked_chain, rtx.blockHash, rtx.nIndex);
    result.time_received = rtx.nTimeReceived;
    result.lock_time = 0; // TODO
    result.is_final = true; // TODO
    result.is_trusted = wallet.IsTrusted(locked_chain, hash, rtx.blockHash);
    result.is_abandoned = rtx.IsAbandoned();
    result.is_coinbase = false;
    result.is_in_main_chain = result.depth_in_main_chain > 0;
    return result;
}

//! Construct wallet TxOut struct.
WalletTxOut MakeWalletTxOut(interfaces::Chain::Lock& locked_chain,
    CWallet& wallet,
    const CWalletTx& wtx,
    int n,
    int depth) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxOut result;
    result.txout.nValue = wtx.tx->vpout[n]->GetValue();
    result.txout.scriptPubKey = *wtx.tx->vpout[n]->GetPScriptPubKey();
    result.time = wtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(locked_chain, wtx.GetHash(), n);
    return result;
}

WalletTxOut MakeWalletTxOut(interfaces::Chain::Lock& locked_chain,
    CHDWallet &wallet, const uint256 &hash, const CTransactionRecord &rtx, int n, int depth) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxOut result;
    const COutputRecord *oR = rtx.GetOutput(n);
    if (!oR) {
        return result;
    }
    result.txout.nValue = oR->nValue;
    result.txout.scriptPubKey = oR->scriptPubKey;
    result.time = rtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(locked_chain, hash, n);
    return result;
}

class WalletImpl : public Wallet
{
public:
    explicit WalletImpl(const std::shared_ptr<CWallet>& wallet) : m_wallet(wallet)
    {
        if (::IsParticlWallet(wallet.get())) {
            m_wallet_part = GetParticlWallet(wallet.get());
        }
    }

    bool encryptWallet(const SecureString& wallet_passphrase) override
    {
        return m_wallet->EncryptWallet(wallet_passphrase);
    }
    bool isCrypted() override { return m_wallet->IsCrypted(); }
    bool lock() override { return m_wallet->Lock(); }
    bool unlock(const SecureString& wallet_passphrase, bool for_staking_only) override
    {
        if (!m_wallet->Unlock(wallet_passphrase)) {
            return false;
        }
        if (m_wallet_part) {
            m_wallet_part->fUnlockForStakingOnly = for_staking_only;
        }
        return true;
    }
    bool isLocked() override { return m_wallet->IsLocked(); }
    bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) override
    {
        return m_wallet->ChangeWalletPassphrase(old_wallet_passphrase, new_wallet_passphrase);
    }
    void abortRescan() override { m_wallet->AbortRescan(); }
    bool backupWallet(const std::string& filename) override { return m_wallet->BackupWallet(filename); }
    std::string getWalletName() override { return m_wallet->GetName(); }
    bool getNewDestination(const OutputType type, const std::string label, CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        std::string error;
        return m_wallet->GetNewDestination(type, label, dest, error);
    }
    bool getPubKey(const CKeyID& address, CPubKey& pub_key) override { return m_wallet->GetPubKey(address, pub_key); }
    bool getPrivKey(const CKeyID& address, CKey& key) override { return m_wallet->GetKey(address, key); }
    bool isSpendable(const CTxDestination& dest) override { return IsMine(*m_wallet, dest) & ISMINE_SPENDABLE; }
    bool haveWatchOnly() override { return m_wallet->HaveWatchOnly(); };
    bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) override
    {
        return m_wallet->SetAddressBook(dest, name, purpose);
    }
    bool delAddressBook(const CTxDestination& dest) override
    {
        return m_wallet->DelAddressBook(dest);
    }
    bool getAddress(const CTxDestination& dest,
        std::string* name,
        isminetype* is_mine,
        std::string* purpose) override
    {
        LOCK(m_wallet->cs_wallet);
        auto it = m_wallet->mapAddressBook.find(dest);
        if (it == m_wallet->mapAddressBook.end()) {
            return false;
        }
        if (name) {
            *name = it->second.name;
        }
        if (is_mine) {
            *is_mine = IsMine(*m_wallet, dest);
        }
        if (purpose) {
            *purpose = it->second.purpose;
        }
        return true;
    }
    std::vector<WalletAddress> getAddresses() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletAddress> result;
        for (const auto& item : m_wallet->mapAddressBook) {
            result.emplace_back(item.first, IsMine(*m_wallet, item.first), item.second.name, item.second.purpose, item.second.fBech32);
        }
        return result;
    }
    void learnRelatedScripts(const CPubKey& key, OutputType type) override { m_wallet->LearnRelatedScripts(key, type); }
    bool addDestData(const CTxDestination& dest, const std::string& key, const std::string& value) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->AddDestData(dest, key, value);
    }
    bool eraseDestData(const CTxDestination& dest, const std::string& key) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->EraseDestData(dest, key);
    }
    std::vector<std::string> getDestValues(const std::string& prefix) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetDestValues(prefix);
    }
    void lockCoin(const COutPoint& output) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->LockCoin(output);
    }
    void unlockCoin(const COutPoint& output) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->UnlockCoin(output);
    }
    bool isLockedCoin(const COutPoint& output) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsLockedCoin(output.hash, output.n);
    }
    void listLockedCoins(std::vector<COutPoint>& outputs) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->ListLockedCoins(outputs);
    }
    CTransactionRef createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        std::string& fail_reason) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        CTransactionRef tx;
        if (!m_wallet->CreateTransaction(*locked_chain, recipients, tx, fee, change_pos,
                fail_reason, coin_control, sign)) {
            return {};
        }
        return tx;
    }
    bool commitTransaction(CTransactionRef tx,
        WalletValueMap value_map,
        WalletOrderForm order_form,
        std::string& reject_reason) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        CValidationState state;
        if (!m_wallet->CommitTransaction(std::move(tx), std::move(value_map), std::move(order_form), state)) {
            reject_reason = state.GetRejectReason();
            return false;
        }
        return true;
    }
    bool transactionCanBeAbandoned(const uint256& txid) override { return m_wallet->TransactionCanBeAbandoned(txid); }
    bool abandonTransaction(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->AbandonTransaction(*locked_chain, txid);
    }
    bool transactionCanBeBumped(const uint256& txid) override
    {
        return feebumper::TransactionCanBeBumped(m_wallet.get(), txid);
    }
    bool createBumpTransaction(const uint256& txid,
        const CCoinControl& coin_control,
        CAmount total_fee,
        std::vector<std::string>& errors,
        CAmount& old_fee,
        CAmount& new_fee,
        CMutableTransaction& mtx) override
    {
        if (total_fee > 0 || ::IsParticlWallet(m_wallet.get())) {
            return feebumper::CreateTotalBumpTransaction(m_wallet.get(), txid, coin_control, total_fee, errors, old_fee, new_fee, mtx) ==
                feebumper::Result::OK;
        } else {
            return feebumper::CreateRateBumpTransaction(m_wallet.get(), txid, coin_control, errors, old_fee, new_fee, mtx) ==
                feebumper::Result::OK;
        }
    }
    bool signBumpTransaction(CMutableTransaction& mtx) override { return feebumper::SignTransaction(m_wallet.get(), mtx); }
    bool commitBumpTransaction(const uint256& txid,
        CMutableTransaction&& mtx,
        std::vector<std::string>& errors,
        uint256& bumped_txid) override
    {
        return feebumper::CommitTransaction(m_wallet.get(), txid, std::move(mtx), errors, bumped_txid) ==
               feebumper::Result::OK;
    }
    CTransactionRef getTx(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return mi->second.tx;
        }
        return {};
    }
    WalletTx getWalletTx(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return MakeWalletTx(*locked_chain, *m_wallet, mi->second);
        }

        if (m_wallet_part) {
            const auto mi = m_wallet_part->mapRecords.find(txid);
            if (mi != m_wallet_part->mapRecords.end()) {
                return MakeWalletTx(*m_wallet_part, mi);
            }
        }

        return {};
    }
    std::vector<WalletTx> getWalletTxs() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTx> result;
        result.reserve(m_wallet->mapWallet.size());
        for (const auto& entry : m_wallet->mapWallet) {
            result.emplace_back(MakeWalletTx(*locked_chain, *m_wallet, entry.second));
        }
        if (m_wallet_part) {
            for (auto mi = m_wallet_part->mapRecords.begin(); mi != m_wallet_part->mapRecords.end(); mi++) {
                result.emplace_back(MakeWalletTx(*m_wallet_part, mi));
            }
        }

        return result;
    }
    bool tryGetTxStatus(const uint256& txid,
        interfaces::WalletTxStatus& tx_status,
        int& num_blocks,
        int64_t& block_time) override
    {
        auto locked_chain = m_wallet->chain().lock(true /* try_lock */);
        if (!locked_chain) {
            return false;
        }
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi == m_wallet->mapWallet.end()) {

            if (m_wallet_part) {
                auto mi = m_wallet_part->mapRecords.find(txid);
                if (mi != m_wallet_part->mapRecords.end()) {
                    num_blocks = locked_chain->getHeight().get_value_or(-1);
                    tx_status = MakeWalletTxStatus(*locked_chain, *m_wallet_part, mi->first, mi->second);
                    return true;
                }
            }
            return false;
        }
        if (Optional<int> height = locked_chain->getHeight()) {
            num_blocks = *height;
            block_time = locked_chain->getBlockTime(*height);
        } else {
            num_blocks = -1;
            block_time = -1;
        }
        tx_status = MakeWalletTxStatus(*locked_chain, mi->second);
        return true;
    }
    WalletTx getWalletTxDetails(const uint256& txid,
        WalletTxStatus& tx_status,
        WalletOrderForm& order_form,
        bool& in_mempool,
        int& num_blocks) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            num_blocks = locked_chain->getHeight().get_value_or(-1);
            in_mempool = mi->second.InMempool();
            order_form = mi->second.vOrderForm;
            tx_status = MakeWalletTxStatus(*locked_chain, mi->second);
            return MakeWalletTx(*locked_chain, *m_wallet, mi->second);
        }
        if (m_wallet_part) {
            auto mi = m_wallet_part->mapRecords.find(txid);
            if (mi != m_wallet_part->mapRecords.end()) {
                num_blocks = locked_chain->getHeight().get_value_or(-1);
                in_mempool = m_wallet_part->InMempool(mi->first);
                order_form = {};
                tx_status = MakeWalletTxStatus(*locked_chain, *m_wallet_part, mi->first, mi->second);
                return MakeWalletTx(*m_wallet_part, mi);
            }
        }
        return {};
    }
    WalletBalances getBalances() override
    {
        WalletBalances result;

        if (m_wallet_part) {
            CHDWalletBalances bal;
            if (!m_wallet_part->GetBalances(bal)) {
                return result;
            }

            result.balance = bal.nPart;
            result.balanceStaked = bal.nPartStaked;
            result.balanceBlind = bal.nBlind;
            result.balanceAnon = bal.nAnon;
            result.unconfirmed_balance = bal.nPartUnconf + bal.nBlindUnconf + bal.nAnonUnconf;
            result.immature_balance = bal.nPartImmature;
            result.immature_anon_balance = bal.nAnonImmature;
            result.have_watch_only = bal.nPartWatchOnly || bal.nPartWatchOnlyUnconf || bal.nPartWatchOnlyStaked;
            if (result.have_watch_only) {
                result.watch_only_balance = bal.nPartWatchOnly;
                result.unconfirmed_watch_only_balance = bal.nPartWatchOnlyUnconf;
                //result.immature_watch_only_balance = m_wallet.GetImmatureWatchOnlyBalance();
                result.balanceWatchStaked = bal.nPartWatchOnlyStaked;
            }

            return result;
        }

        const auto bal = m_wallet->GetBalance();

        result.balance = bal.m_mine_trusted;
        result.unconfirmed_balance = bal.m_mine_untrusted_pending;
        result.immature_balance = bal.m_mine_immature;
        result.have_watch_only = m_wallet->HaveWatchOnly();
        if (result.have_watch_only) {
            result.watch_only_balance = bal.m_watchonly_trusted;
            result.unconfirmed_watch_only_balance = bal.m_watchonly_untrusted_pending;
            result.immature_watch_only_balance = bal.m_watchonly_immature;
        }
        return result;
    }
    bool tryGetBalances(WalletBalances& balances, bool skip_height_check, int cached_blocks, int& num_blocks) override
    {
        auto locked_chain = m_wallet->chain().lock(true /* try_lock */);
        if (!locked_chain) return false;

        num_blocks = locked_chain->getHeight().get_value_or(-1);
        if (!skip_height_check && num_blocks == cached_blocks) {
            return false;
        }

        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        balances = getBalances();
        num_blocks = locked_chain->getHeight().get_value_or(-1);
        return true;
    }
    CAmount getBalance() override { return m_wallet->GetBalance().m_mine_trusted; }
    CAmount getAvailableBalance(const CCoinControl& coin_control) override
    {
        return m_wallet->GetAvailableBalance(&coin_control);
    }
    isminetype txinIsMine(const CTxIn& txin) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txin);
    }
    isminetype txoutIsMine(const CTxOut& txout) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txout);
    }
    CAmount getDebit(const CTxIn& txin, isminefilter filter) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetDebit(txin, filter);
    }
    CAmount getCredit(const CTxOut& txout, isminefilter filter) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetCredit(txout, filter);
    }
    CoinsList listCoins(OutputTypes nType) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        CoinsList result;
        if (m_wallet_part
            && nType != OUTPUT_STANDARD) {
            for (const auto& entry : m_wallet_part->ListCoins(*locked_chain, nType)) {
                auto& group = result[entry.first];
                for (const auto& coin : entry.second) {
                    group.emplace_back(
                        COutPoint(coin.rtx->first, coin.i), MakeWalletTxOut(*locked_chain, *m_wallet_part, coin.txhash, coin.rtx->second, coin.i, coin.nDepth));
                }
            }
            return result;
        }

        for (const auto& entry : m_wallet->ListCoins(*locked_chain)) {
            auto& group = result[entry.first];
            for (const auto& coin : entry.second) {
                group.emplace_back(COutPoint(coin.tx->GetHash(), coin.i),
                    MakeWalletTxOut(*locked_chain, *m_wallet, *coin.tx, coin.i, coin.nDepth));
            }
        }
        return result;
    }
    std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTxOut> result;
        result.reserve(outputs.size());
        for (const auto& output : outputs) {
            result.emplace_back();
            auto it = m_wallet->mapWallet.find(output.hash);
            if (it != m_wallet->mapWallet.end()) {
                int depth = it->second.GetDepthInMainChain(*locked_chain);
                if (depth >= 0) {
                    result.back() = MakeWalletTxOut(*locked_chain, *m_wallet, it->second, output.n, depth);
                }
            } else
            if (m_wallet_part) {
                const auto mi = m_wallet_part->mapRecords.find(output.hash);
                if (mi != m_wallet_part->mapRecords.end()) {
                    const auto &rtx = mi->second;
                    int depth = m_wallet_part->GetDepthInMainChain(*locked_chain, rtx.blockHash, rtx.nIndex);
                    if (depth >= 0) {
                        result.back() = MakeWalletTxOut(*locked_chain, *m_wallet_part, output.hash, rtx, output.n, depth);
                    }
                }
            }
        }
        return result;
    }
    CAmount getRequiredFee(unsigned int tx_bytes) override { return GetRequiredFee(*m_wallet, tx_bytes); }
    CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) override
    {
        FeeCalculation fee_calc;
        CAmount result;
        result = GetMinimumFee(*m_wallet, tx_bytes, coin_control, &fee_calc);
        if (returned_target) *returned_target = fee_calc.returnedTarget;
        if (reason) *reason = fee_calc.reason;
        return result;
    }
    unsigned int getConfirmTarget() override { return m_wallet->m_confirm_target; }
    bool hdEnabled() override { return m_wallet->IsHDEnabled(); }
    bool canGetAddresses() override { return m_wallet->CanGetAddresses(); }
    bool IsWalletFlagSet(uint64_t flag) override { return m_wallet->IsWalletFlagSet(flag); }
    OutputType getDefaultAddressType() override { return m_wallet->m_default_address_type; }
    OutputType getDefaultChangeType() override { return m_wallet->m_default_change_type; }
    CAmount getDefaultMaxTxFee() override { return m_wallet->m_default_max_tx_fee; }
    void remove() override
    {
        RemoveWallet(m_wallet);
        if (m_wallet_part) {
            smsgModule.WalletUnloaded(m_wallet_part);
            m_wallet_part = nullptr;
            RestartStakingThreads();
        }
    }
    std::unique_ptr<Handler> handleUnload(UnloadFn fn) override
    {
        return MakeHandler(m_wallet->NotifyUnload.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeHandler(m_wallet->ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyStatusChanged.connect([fn](CWallet*) { fn(); }));
    }
    std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyAddressBookChanged.connect(
            [fn](CWallet*, const CTxDestination& address, const std::string& label, bool is_mine,
                const std::string& purpose, ChangeType status) { fn(address, label, is_mine, purpose, status); }));
    }
    std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyTransactionChanged.connect(
            [fn](CWallet*, const uint256& txid, ChangeType status) { fn(txid, status); }));
    }
    std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyWatchonlyChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyCanGetAddressesChanged.connect(fn));
    }

    std::shared_ptr<CWallet> m_wallet;

    std::unique_ptr<Handler> handleReservedBalanceChanged(ReservedBalanceChangedFn fn) override
    {
        return MakeHandler(m_wallet_part->NotifyReservedBalanceChanged.connect(fn));
    }

    bool IsParticlWallet() override
    {
        return m_wallet_part;
    }

    CAmount getReserveBalance() override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->nReserveBalance;
    }

    bool ownDestination(const CTxDestination &dest) override
    {
        if (!m_wallet_part)
            return false;
        return m_wallet_part->HaveAddress(dest);
    }

    bool isUnlockForStakingOnlySet() override
    {
        if (!m_wallet_part)
            return false;
        return m_wallet_part->fUnlockForStakingOnly;
    }

    CAmount getAvailableAnonBalance(const CCoinControl& coin_control) override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->GetAvailableAnonBalance(&coin_control);
    }

    CAmount getAvailableBlindBalance(const CCoinControl& coin_control) override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->GetAvailableBlindBalance(&coin_control);
    }

    CHDWallet *getParticlWallet() override
    {
        return m_wallet_part;
    }

    bool setReserveBalance(CAmount nValue) override
    {
        if (!m_wallet_part)
            return false;
        return m_wallet_part->SetReserveBalance(nValue);
    }

    void lockWallet() override
    {
        if (!m_wallet_part)
            return;
        ::LockWallet(m_wallet_part);
    }

    bool setUnlockedForStaking() override
    {
        if (!m_wallet_part)
            return false;
        if (m_wallet_part->IsLocked())
            return false;
        m_wallet_part->fUnlockForStakingOnly = true;
        return true;
    }

    bool isDefaultAccountSet() override
    {
        if (!m_wallet_part)
            return false;
        return (!m_wallet_part->idDefaultAccount.IsNull());
    }

    CAmount getCredit(const CTxOutBase *txout, isminefilter filter) override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->GetCredit(txout, filter);
    }

    isminetype txoutIsMine(const CTxOutBase *txout) override
    {
        if (!m_wallet_part)
            return ISMINE_NO;
        return m_wallet_part->IsMine(txout);
    }

    CHDWallet *m_wallet_part = nullptr;
};

class WalletClientImpl : public ChainClient
{
public:
    WalletClientImpl(Chain& chain, std::vector<std::string> wallet_filenames)
        : m_chain(chain), m_wallet_filenames(std::move(wallet_filenames))
    {
    }
    void registerRpcs() override {
        RegisterHDWalletRPCCommands(m_chain, m_rpc_handlers);
        return RegisterWalletRPCCommands(m_chain, m_rpc_handlers); }
    bool verify() override { return VerifyWallets(m_chain, m_wallet_filenames); }
    bool load() override { return LoadWallets(m_chain, m_wallet_filenames); }
    void start(CScheduler& scheduler) override { return StartWallets(scheduler); }
    void flush() override { return FlushWallets(); }
    void stop() override { return StopWallets(); }
    ~WalletClientImpl() override { UnloadWallets(); }

    Chain& m_chain;
    std::vector<std::string> m_wallet_filenames;
    std::vector<std::unique_ptr<Handler>> m_rpc_handlers;
};

} // namespace

std::unique_ptr<Wallet> MakeWallet(const std::shared_ptr<CWallet>& wallet) { return wallet ? MakeUnique<WalletImpl>(wallet) : nullptr; }

std::unique_ptr<ChainClient> MakeWalletClient(Chain& chain, std::vector<std::string> wallet_filenames)
{
    return MakeUnique<WalletClientImpl>(chain, std::move(wallet_filenames));
}

} // namespace interfaces
