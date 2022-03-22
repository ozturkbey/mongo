/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "mongo/db/fle_crud.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_api.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_resource_yielder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace {

std::shared_ptr<ThreadPool> _fleCrudthreadPool;

ThreadPool::Options getThreadPoolOptions() {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "FLECrud";
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;

    // SEPTransactionClient::runCommand manages the client itself so do not create one via
    // onCreateThread
    return tpOptions;
}

void setMongosFieldsInReply(OperationContext* opCtx, write_ops::WriteCommandReplyBase* replyBase) {
    // Set these fields only if not set
    if (replyBase->getOpTime().has_value() && replyBase->getElectionId().has_value()) {
        return;
    }

    // Undocumented repl fields that mongos depends on.
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    const auto replMode = replCoord->getReplicationMode();
    if (replMode != repl::ReplicationCoordinator::modeNone) {

        replyBase->setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
        replyBase->setElectionId(replCoord->getElectionId());
    }
}


class FLEMongoDResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext* opCtx) override {
        // We're about to block. Check back in the session so that it's available to other
        // threads. Note that we may block on a request to _ourselves_, meaning that we may have to
        // wait for another thread which will use the same session. This step is necessary
        // to prevent deadlocks.

        Session* const session = OperationContextSession::get(opCtx);
        if (session) {
            if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
                txnParticipant.stashTransactionResources(opCtx);
            }

            MongoDOperationContextSession::checkIn(opCtx);
        }
        _yielded = (session != nullptr);
    }

    void unyield(OperationContext* opCtx) override {
        if (_yielded) {
            // This may block on a sub-operation on this node finishing. It's possible that while
            // blocked on the network layer, another shard could have responded, theoretically
            // unblocking this thread of execution. However, we must wait until the child operation
            // on this shard finishes so we can get the session back. This may limit the throughput
            // of the operation, but it's correct.
            MongoDOperationContextSession::checkOut(opCtx);

            if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
                // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code
                // which relies on this parameter does not distinguish/care about the difference so
                // we simply always pass 'aggregate'.
                //
                // Catch NoSuchTransaction which happens when the transaction is aborted by an
                // unrelated error If this error is not caught, then a user error like DuplicateKey
                // gets ignored for NoSuchTransaction
                try {
                    txnParticipant.unstashTransactionResources(opCtx, "aggregate");
                } catch (ExceptionFor<ErrorCodes::NoSuchTransaction>&) {
                }
            }
        }
    }

private:
    bool _yielded = false;
};

std::shared_ptr<txn_api::TransactionWithRetries> getTransactionWithRetriesForMongoD(
    OperationContext* opCtx) {
    return std::make_shared<txn_api::TransactionWithRetries>(
        opCtx, _fleCrudthreadPool, std::make_unique<FLEMongoDResourceYielder>());
}

}  // namespace


void startFLECrud(ServiceContext* serviceContext) {
    // FLE crud is only supported on replica sets so no reason to start thread pool on standalones
    if (repl::ReplicationCoordinator::get(serviceContext)->getReplicationMode() ==
        repl::ReplicationCoordinator::modeNone) {
        return;
    }

    _fleCrudthreadPool = std::make_shared<ThreadPool>(getThreadPoolOptions());
    _fleCrudthreadPool->startup();
}

void stopFLECrud() {
    // Check if it was started
    if (_fleCrudthreadPool.get() != nullptr) {
        _fleCrudthreadPool->shutdown();
    }
}

FLEBatchResult processFLEInsert(OperationContext* opCtx,
                                const write_ops::InsertCommandRequest& insertRequest,
                                write_ops::InsertCommandReply* insertReply) {

    uassert(6371602,
            "Encrypted index operations are only supported on replica sets",
            repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet);

    auto [batchResult, insertReplyReturn] =
        processInsert(opCtx, insertRequest, &getTransactionWithRetriesForMongoD);

    if (batchResult == FLEBatchResult::kNotProcessed) {
        return FLEBatchResult::kNotProcessed;
    }

    *insertReply = insertReplyReturn;

    setMongosFieldsInReply(opCtx, &insertReply->getWriteCommandReplyBase());

    return FLEBatchResult::kProcessed;
}

}  // namespace mongo