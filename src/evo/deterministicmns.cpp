// Copyright (c) 2018 The Ctp Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "deterministicmns.h"
#include "specialtx.h"

#include "base58.h"
#include "chainparams.h"
#include "core_io.h"
#include "script/standard.h"
#include "spork.h"
#include "validation.h"
#include "validationinterface.h"

#include <univalue.h>

static const std::string DB_LIST_SNAPSHOT = "dmn_S";
static const std::string DB_LIST_DIFF = "dmn_D";

CDeterministicMNManager* deterministicMNManager;

std::string CDeterministicMNState::ToString() const
{
    CTxDestination dest;
    std::string payoutAddress = "unknown";
    std::string operatorRewardAddress = "none";
    if (ExtractDestination(scriptPayout, dest)) {
        payoutAddress = CBitcoinAddress(dest).ToString();
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        operatorRewardAddress = CBitcoinAddress(dest).ToString();
    }

    return strprintf("CDeterministicMNState(nRegisteredHeight=%d, nLastPaidHeight=%d, nPoSePenalty=%d, nPoSeRevivedHeight=%d, nPoSeBanHeight=%d, nRevocationReason=%d, "
        "keyIDOwner=%s, pubKeyOperator=%s, keyIDVoting=%s, addr=%s, payoutAddress=%s, operatorRewardAddress=%s)",
        nRegisteredHeight, nLastPaidHeight, nPoSePenalty, nPoSeRevivedHeight, nPoSeBanHeight, nRevocationReason,
        keyIDOwner.ToString(), pubKeyOperator.ToString(), keyIDVoting.ToString(), addr.ToStringIPPort(false), payoutAddress, operatorRewardAddress);
}

void CDeterministicMNState::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.push_back(Pair("registeredHeight", nRegisteredHeight));
    obj.push_back(Pair("lastPaidHeight", nLastPaidHeight));
    obj.push_back(Pair("PoSePenalty", nPoSePenalty));
    obj.push_back(Pair("PoSeRevivedHeight", nPoSeRevivedHeight));
    obj.push_back(Pair("PoSeBanHeight", nPoSeBanHeight));
    obj.push_back(Pair("revocationReason", nRevocationReason));
    obj.push_back(Pair("keyIDOwner", keyIDOwner.ToString()));
    obj.push_back(Pair("pubKeyOperator", pubKeyOperator.ToString()));
    obj.push_back(Pair("keyIDVoting", keyIDVoting.ToString()));
    obj.push_back(Pair("addr", addr.ToStringIPPort(false)));

    CTxDestination dest;
    if (ExtractDestination(scriptPayout, dest)) {
        CBitcoinAddress bitcoinAddress(dest);
        obj.push_back(Pair("payoutAddress", bitcoinAddress.ToString()));
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        CBitcoinAddress bitcoinAddress(dest);
        obj.push_back(Pair("operatorRewardAddress", bitcoinAddress.ToString()));
    }
}

std::string CDeterministicMN::ToString() const
{
    return strprintf("CDeterministicMN(proTxHash=%s, collateralOutpoint=%s, nOperatorReward=%f, state=%s", proTxHash.ToString(), collateralOutpoint.ToStringShort(), (double)nOperatorReward / 100, pdmnState->ToString());
}

void CDeterministicMN::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();

    UniValue stateObj;
    pdmnState->ToJson(stateObj);

    obj.push_back(Pair("proTxHash", proTxHash.ToString()));
    obj.push_back(Pair("collateralHash", collateralOutpoint.hash.ToString()));
    obj.push_back(Pair("collateralIndex", (int)collateralOutpoint.n));
    obj.push_back(Pair("operatorReward", (double)nOperatorReward / 100));
    obj.push_back(Pair("state", stateObj));
}

bool CDeterministicMNList::IsMNValid(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return false;
    }
    return IsMNValid(*p);
}

bool CDeterministicMNList::IsMNPoSeBanned(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return false;
    }
    return IsMNPoSeBanned(*p);
}

bool CDeterministicMNList::IsMNValid(const CDeterministicMNCPtr& dmn) const
{
    return !IsMNPoSeBanned(dmn);
}

bool CDeterministicMNList::IsMNPoSeBanned(const CDeterministicMNCPtr& dmn) const
{
    assert(dmn);
    const CDeterministicMNState& state = *dmn->pdmnState;
    return state.nPoSeBanHeight != -1;
}

CDeterministicMNCPtr CDeterministicMNList::GetMN(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return nullptr;
    }
    return *p;
}

CDeterministicMNCPtr CDeterministicMNList::GetValidMN(const uint256& proTxHash) const
{
    auto dmn = GetMN(proTxHash);
    if (dmn && !IsMNValid(dmn)) {
        return nullptr;
    }
    return dmn;
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByOperatorKey(const CBLSPublicKey& pubKey)
{
    for (const auto& p : mnMap) {
        if (p.second->pdmnState->pubKeyOperator == pubKey) {
            return p.second;
        }
    }
    return nullptr;
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByCollateral(const COutPoint& collateralOutpoint) const
{
    return GetUniquePropertyMN(collateralOutpoint);
}

static int CompareByLastPaid_GetHeight(const CDeterministicMN& dmn)
{
    int height = dmn.pdmnState->nLastPaidHeight;
    if (dmn.pdmnState->nPoSeRevivedHeight != -1 && dmn.pdmnState->nPoSeRevivedHeight > height) {
        height = dmn.pdmnState->nPoSeRevivedHeight;
    } else if (height == 0) {
        height = dmn.pdmnState->nRegisteredHeight;
    }
    return height;
}

static bool CompareByLastPaid(const CDeterministicMN& _a, const CDeterministicMN& _b)
{
    int ah = CompareByLastPaid_GetHeight(_a);
    int bh = CompareByLastPaid_GetHeight(_b);
    if (ah == bh) {
        return _a.proTxHash < _b.proTxHash;
    } else {
        return ah < bh;
    }
}
static bool CompareByLastPaid(const CDeterministicMNCPtr& _a, const CDeterministicMNCPtr& _b)
{
    return CompareByLastPaid(*_a, *_b);
}

CDeterministicMNCPtr CDeterministicMNList::GetMNPayee() const
{
    if (mnMap.size() == 0) {
        return nullptr;
    }

    CDeterministicMNCPtr best;
    ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
        if (!best || CompareByLastPaid(dmn, best)) {
            best = dmn;
        }
    });

    return best;
}

std::vector<CDeterministicMNCPtr> CDeterministicMNList::GetProjectedMNPayees(int nCount) const
{
    std::vector<CDeterministicMNCPtr> result;
    result.reserve(nCount);

    CDeterministicMNList tmpMNList = *this;
    for (int h = nHeight; h < nHeight + nCount; h++) {
        tmpMNList.SetHeight(h);

        CDeterministicMNCPtr payee = tmpMNList.GetMNPayee();
        // push the original MN object instead of the one from the temporary list
        result.push_back(GetMN(payee->proTxHash));

        CDeterministicMNStatePtr newState = std::make_shared<CDeterministicMNState>(*payee->pdmnState);
        newState->nLastPaidHeight = h;
        tmpMNList.UpdateMN(payee->proTxHash, newState);
    }

    return result;
}

CDeterministicMNListDiff CDeterministicMNList::BuildDiff(const CDeterministicMNList& to) const
{
    CDeterministicMNListDiff diffRet;
    diffRet.prevBlockHash = blockHash;
    diffRet.blockHash = to.blockHash;
    diffRet.nHeight = to.nHeight;

    for (const auto& p : to.mnMap) {
        const auto fromPtr = mnMap.find(p.first);
        if (fromPtr == nullptr) {
            diffRet.addedMNs.emplace(p.first, p.second);
        } else if (*p.second->pdmnState != *(*fromPtr)->pdmnState) {
            diffRet.updatedMNs.emplace(p.first, p.second->pdmnState);
        }
    }
    for (const auto& p : mnMap) {
        const auto toPtr = to.mnMap.find(p.first);
        if (toPtr == nullptr) {
            diffRet.removedMns.insert(p.first);
        }
    }

    return diffRet;
}

CDeterministicMNList CDeterministicMNList::ApplyDiff(const CDeterministicMNListDiff& diff) const
{
    assert(diff.prevBlockHash == blockHash && diff.nHeight == nHeight + 1);

    CDeterministicMNList result = *this;
    result.blockHash = diff.blockHash;
    result.nHeight = diff.nHeight;

    for (const auto& hash : diff.removedMns) {
        result.RemoveMN(hash);
    }
    for (const auto& p : diff.addedMNs) {
        result.AddMN(p.second);
    }
    for (const auto& p : diff.updatedMNs) {
        result.UpdateMN(p.first, p.second);
    }

    return result;
}

void CDeterministicMNList::AddMN(const CDeterministicMNCPtr& dmn)
{
    assert(!mnMap.find(dmn->proTxHash));
    mnMap = mnMap.set(dmn->proTxHash, dmn);
    AddUniqueProperty(dmn, dmn->collateralOutpoint);
    AddUniqueProperty(dmn, dmn->pdmnState->addr);
    AddUniqueProperty(dmn, dmn->pdmnState->keyIDOwner);
    AddUniqueProperty(dmn, dmn->pdmnState->pubKeyOperator);
}

void CDeterministicMNList::UpdateMN(const uint256& proTxHash, const CDeterministicMNStateCPtr& pdmnState)
{
    auto oldDmn = mnMap.find(proTxHash);
    assert(oldDmn != nullptr);
    auto dmn = std::make_shared<CDeterministicMN>(**oldDmn);
    auto oldState = dmn->pdmnState;
    dmn->pdmnState = pdmnState;
    mnMap = mnMap.set(proTxHash, dmn);

    UpdateUniqueProperty(dmn, oldState->addr, pdmnState->addr);
    UpdateUniqueProperty(dmn, oldState->keyIDOwner, pdmnState->keyIDOwner);
    UpdateUniqueProperty(dmn, oldState->pubKeyOperator, pdmnState->pubKeyOperator);
}

void CDeterministicMNList::RemoveMN(const uint256& proTxHash)
{
    auto dmn = GetMN(proTxHash);
    assert(dmn != nullptr);
    DeleteUniqueProperty(dmn, dmn->collateralOutpoint);
    DeleteUniqueProperty(dmn, dmn->pdmnState->addr);
    DeleteUniqueProperty(dmn, dmn->pdmnState->keyIDOwner);
    DeleteUniqueProperty(dmn, dmn->pdmnState->pubKeyOperator);
    mnMap = mnMap.erase(proTxHash);
}

CDeterministicMNManager::CDeterministicMNManager(CEvoDB& _evoDb) :
    evoDb(_evoDb)
{
}

bool CDeterministicMNManager::ProcessBlock(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& _state)
{
    LOCK(cs);

    int nHeight = pindexPrev->nHeight + 1;

    CDeterministicMNList newList;
    if (!BuildNewListFromBlock(block, pindexPrev, _state, newList)) {
        return false;
    }

    if (newList.GetHeight() == -1) {
        newList.SetHeight(nHeight);
    }

    newList.SetBlockHash(block.GetHash());

    CDeterministicMNList oldList = GetListForBlock(pindexPrev->GetBlockHash());
    CDeterministicMNListDiff diff = oldList.BuildDiff(newList);

    evoDb.Write(std::make_pair(DB_LIST_DIFF, diff.blockHash), diff);
    if ((nHeight % SNAPSHOT_LIST_PERIOD) == 0) {
        evoDb.Write(std::make_pair(DB_LIST_SNAPSHOT, diff.blockHash), newList);
        LogPrintf("CDeterministicMNManager::%s -- Wrote snapshot. nHeight=%d, mapCurMNs.allMNsCount=%d\n",
            __func__, nHeight, newList.GetAllMNsCount());
    }

    if (nHeight == GetSpork15Value()) {
        LogPrintf("CDeterministicMNManager::%s -- spork15 is active now. nHeight=%d\n", __func__, nHeight);
    }

    CleanupCache(nHeight);

    return true;
}

bool CDeterministicMNManager::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    LOCK(cs);

    int nHeight = pindex->nHeight;
    uint256 blockHash = block.GetHash();

    evoDb.Erase(std::make_pair(DB_LIST_DIFF, blockHash));
    evoDb.Erase(std::make_pair(DB_LIST_SNAPSHOT, blockHash));
    mnListsCache.erase(blockHash);

    if (nHeight == GetSpork15Value()) {
        LogPrintf("CDeterministicMNManager::%s -- spork15 is not active anymore. nHeight=%d\n", __func__, nHeight);
    }

    return true;
}

void CDeterministicMNManager::UpdatedBlockTip(const CBlockIndex* pindex)
{
    LOCK(cs);

    tipHeight = pindex->nHeight;
    tipBlockHash = pindex->GetBlockHash();
}

bool CDeterministicMNManager::BuildNewListFromBlock(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& _state, CDeterministicMNList& mnListRet)
{
    AssertLockHeld(cs);

    int nHeight = pindexPrev->nHeight + 1;

    CDeterministicMNList oldList = GetListForBlock(pindexPrev->GetBlockHash());
    CDeterministicMNList newList = oldList;
    newList.SetBlockHash(uint256()); // we can't know the final block hash, so better not return a (invalid) block hash
    newList.SetHeight(nHeight);

    auto payee = oldList.GetMNPayee();

    // we skip the coinbase
    for (int i = 1; i < (int)block.vtx.size(); i++) {
        const CTransaction& tx = *block.vtx[i];

        // check if any existing MN collateral is spent by this transaction
        for (const auto& in : tx.vin) {
            auto dmn = newList.GetMNByCollateral(in.prevout);
            if (dmn && dmn->collateralOutpoint == in.prevout) {
                newList.RemoveMN(dmn->proTxHash);

                LogPrintf("CDeterministicMNManager::%s -- MN %s removed from list because collateral was spent. collateralOutpoint=%s, nHeight=%d, mapCurMNs.allMNsCount=%d\n",
                    __func__, dmn->proTxHash.ToString(), dmn->collateralOutpoint.ToStringShort(), nHeight, newList.GetAllMNsCount());
            }
        }

        if (tx.nType == TRANSACTION_PROVIDER_REGISTER) {
            CProRegTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            auto dmn = std::make_shared<CDeterministicMN>();
            dmn->proTxHash = tx.GetHash();

            // collateralOutpoint is either pointing to an external collateral or to the ProRegTx itself
            if (proTx.collateralOutpoint.hash.IsNull()) {
                dmn->collateralOutpoint = COutPoint(tx.GetHash(), proTx.collateralOutpoint.n);
            } else {
                dmn->collateralOutpoint = proTx.collateralOutpoint;
            }

            Coin coin;
            if (!proTx.collateralOutpoint.hash.IsNull() && (!GetUTXOCoin(dmn->collateralOutpoint, coin) || coin.out.nValue != 1000 * COIN)) {
                // should actually never get to this point as CheckProRegTx should have handled this case.
                // We do this additional check nevertheless to be 100% sure
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral");
            }

            auto replacedDmn = newList.GetMNByCollateral(dmn->collateralOutpoint);
            if (replacedDmn != nullptr) {
                // This might only happen with a ProRegTx that refers an external collateral
                // In that case the new ProRegTx will replace the old one. This means the old one is removed
                // and the new one is added like a completely fresh one, which is also at the bottom of the payment list
                newList.RemoveMN(replacedDmn->proTxHash);
                LogPrintf("CDeterministicMNManager::%s -- MN %s removed from list because collateral was used for a new ProRegTx. collateralOutpoint=%s, nHeight=%d, mapCurMNs.allMNsCount=%d\n",
                          __func__, replacedDmn->proTxHash.ToString(), dmn->collateralOutpoint.ToStringShort(), nHeight, newList.GetAllMNsCount());
            }

            if (newList.HasUniqueProperty(proTx.addr)) {
                return _state.DoS(100, false, REJECT_CONFLICT, "bad-protx-dup-addr");
            }
            if (newList.HasUniqueProperty(proTx.keyIDOwner) || newList.HasUniqueProperty(proTx.pubKeyOperator)) {
                return _state.DoS(100, false, REJECT_CONFLICT, "bad-protx-dup-key");
            }

            dmn->nOperatorReward = proTx.nOperatorReward;
            dmn->pdmnState = std::make_shared<CDeterministicMNState>(proTx);

            CDeterministicMNState dmnState = *dmn->pdmnState;
            dmnState.nRegisteredHeight = nHeight;

            if (proTx.addr == CService()) {
                // start in banned pdmnState as we need to wait for a ProUpServTx
                dmnState.nPoSeBanHeight = nHeight;
            }

            dmn->pdmnState = std::make_shared<CDeterministicMNState>(dmnState);

            newList.AddMN(dmn);

            LogPrintf("CDeterministicMNManager::%s -- MN %s added at height %d: %s\n",
                __func__, tx.GetHash().ToString(), nHeight, proTx.ToString());
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_SERVICE) {
            CProUpServTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            if (newList.HasUniqueProperty(proTx.addr) && newList.GetUniquePropertyMN(proTx.addr)->proTxHash != proTx.proTxHash) {
                return _state.DoS(100, false, REJECT_CONFLICT, "bad-protx-dup-addr");
            }

            CDeterministicMNCPtr dmn = newList.GetMN(proTx.proTxHash);
            if (!dmn) {
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
            newState->addr = proTx.addr;
            newState->scriptOperatorPayout = proTx.scriptOperatorPayout;

            if (newState->nPoSeBanHeight != -1) {
                // only revive when all keys are set
                if (newState->pubKeyOperator.IsValid() && !newState->keyIDVoting.IsNull() && !newState->keyIDOwner.IsNull()) {
                    newState->nPoSeBanHeight = -1;
                    newState->nPoSeRevivedHeight = nHeight;

                    LogPrintf("CDeterministicMNManager::%s -- MN %s revived at height %d\n",
                        __func__, proTx.proTxHash.ToString(), nHeight);
                }
            }

            newList.UpdateMN(proTx.proTxHash, newState);

            LogPrintf("CDeterministicMNManager::%s -- MN %s updated at height %d: %s\n",
                __func__, proTx.proTxHash.ToString(), nHeight, proTx.ToString());
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REGISTRAR) {
            CProUpRegTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            CDeterministicMNCPtr dmn = newList.GetMN(proTx.proTxHash);
            if (!dmn) {
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
            if (newState->pubKeyOperator != proTx.pubKeyOperator) {
                // reset all operator related fields and put MN into PoSe-banned state in case the operator key changes
                newState->ResetOperatorFields();
                newState->BanIfNotBanned(nHeight);
            }
            newState->pubKeyOperator = proTx.pubKeyOperator;
            newState->keyIDVoting = proTx.keyIDVoting;
            newState->scriptPayout = proTx.scriptPayout;

            newList.UpdateMN(proTx.proTxHash, newState);

            LogPrintf("CDeterministicMNManager::%s -- MN %s updated at height %d: %s\n",
                __func__, proTx.proTxHash.ToString(), nHeight, proTx.ToString());
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REVOKE) {
            CProUpRevTx proTx;
            if (!GetTxPayload(tx, proTx)) {
                assert(false); // this should have been handled already
            }

            CDeterministicMNCPtr dmn = newList.GetMN(proTx.proTxHash);
            if (!dmn) {
                return _state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
            newState->ResetOperatorFields();
            newState->BanIfNotBanned(nHeight);
            newState->nRevocationReason = proTx.nReason;

            newList.UpdateMN(proTx.proTxHash, newState);

            LogPrintf("CDeterministicMNManager::%s -- MN %s revoked operator key at height %d: %s\n",
                __func__, proTx.proTxHash.ToString(), nHeight, proTx.ToString());
        }
    }

    // The payee for the current block was determined by the previous block's list but it might have disappeared in the
    // current block. We still pay that MN one last time however.
    if (payee && newList.HasMN(payee->proTxHash)) {
        auto newState = std::make_shared<CDeterministicMNState>(*newList.GetMN(payee->proTxHash)->pdmnState);
        newState->nLastPaidHeight = nHeight;
        newList.UpdateMN(payee->proTxHash, newState);
    }

    mnListRet = std::move(newList);

    return true;
}

CDeterministicMNList CDeterministicMNManager::GetListForBlock(const uint256& blockHash)
{
    LOCK(cs);

    auto it = mnListsCache.find(blockHash);
    if (it != mnListsCache.end()) {
        return it->second;
    }

    uint256 blockHashTmp = blockHash;
    CDeterministicMNList snapshot;
    std::list<CDeterministicMNListDiff> listDiff;

    while (true) {
        // try using cache before reading from disk
        it = mnListsCache.find(blockHashTmp);
        if (it != mnListsCache.end()) {
            snapshot = it->second;
            break;
        }

        if (evoDb.Read(std::make_pair(DB_LIST_SNAPSHOT, blockHashTmp), snapshot)) {
            break;
        }

        CDeterministicMNListDiff diff;
        if (!evoDb.Read(std::make_pair(DB_LIST_DIFF, blockHashTmp), diff)) {
            snapshot = CDeterministicMNList(blockHashTmp, -1);
            break;
        }

        listDiff.emplace_front(diff);
        blockHashTmp = diff.prevBlockHash;
    }

    for (const auto& diff : listDiff) {
        if (diff.HasChanges()) {
            snapshot = snapshot.ApplyDiff(diff);
        } else {
            snapshot.SetBlockHash(diff.blockHash);
            snapshot.SetHeight(diff.nHeight);
        }
    }

    mnListsCache.emplace(blockHash, snapshot);
    return snapshot;
}

CDeterministicMNList CDeterministicMNManager::GetListAtChainTip()
{
    LOCK(cs);
    return GetListForBlock(tipBlockHash);
}

bool CDeterministicMNManager::HasValidMNCollateralAtChainTip(const COutPoint& outpoint)
{
    auto mnList = GetListAtChainTip();
    auto dmn = mnList.GetMNByCollateral(outpoint);
    return dmn && mnList.IsMNValid(dmn);
}

bool CDeterministicMNManager::HasMNCollateralAtChainTip(const COutPoint& outpoint)
{
    auto mnList = GetListAtChainTip();
    auto dmn = mnList.GetMNByCollateral(outpoint);
    return dmn != nullptr;
}

int64_t CDeterministicMNManager::GetSpork15Value()
{
    return sporkManager.GetSporkValue(SPORK_15_DETERMINISTIC_MNS_ENABLED);
}

bool CDeterministicMNManager::IsProTxWithCollateral(const CTransactionRef& tx, uint32_t n)
{
    if (tx->nVersion < 3 || tx->nType != TRANSACTION_PROVIDER_REGISTER) {
        return false;
    }
    CProRegTx proTx;
    if (!GetTxPayload(*tx, proTx)) {
        return false;
    }

    if (!proTx.collateralOutpoint.hash.IsNull()) {
        return false;
    }
    if (proTx.collateralOutpoint.n >= tx->vout.size() || proTx.collateralOutpoint.n != n) {
        return false;
    }
    if (tx->vout[n].nValue != 1000 * COIN) {
        return false;
    }
    return true;
}

bool CDeterministicMNManager::IsDeterministicMNsSporkActive(int nHeight)
{
    LOCK(cs);

    if (nHeight == -1) {
        nHeight = tipHeight;
    }

    int64_t spork15Value = GetSpork15Value();
    return nHeight >= spork15Value;
}

void CDeterministicMNManager::CleanupCache(int nHeight)
{
    AssertLockHeld(cs);

    std::vector<uint256> toDelete;
    for (const auto& p : mnListsCache) {
        if (p.second.GetHeight() + LISTS_CACHE_SIZE < nHeight) {
            toDelete.emplace_back(p.first);
        }
    }
    for (const auto& h : toDelete) {
        mnListsCache.erase(h);
    }
}
