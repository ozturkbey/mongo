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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"

namespace mongo {
namespace {

class ConfigSvrMoveRangeCommand final : public TypedCommand<ConfigSvrMoveRangeCommand> {
public:
    using Request = ConfigsvrMoveRange;

    std::string help() const override {
        return "Internal command only invokable on the config server. Do not call directly. "
               "Requests the balancer to move a range.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandFailed,
                    "Can't run moveRange because the feature is disabled in the current FCV mode",
                    feature_flags::gNoMoreAutoSplitter.isEnabled(
                        serverGlobalParams.featureCompatibility));
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " can only be run on the config server",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            const auto nss = ns();
            const auto& req = request();

            // Set read concern level to local for reads into the config database
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            // Make sure that the `to` shard exists
            uassertStatusOKWithContext(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, req.getToShard()),
                "Could not find destination shard");

            const auto cm =
                Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfo(opCtx, nss);
            const auto chunk = cm.findIntersectingChunkWithSimpleCollation(req.getMin());

            bool validBounds = req.getMin().woCompare(chunk.getMin()) == 0 &&
                req.getMax().woCompare(chunk.getMax()) == 0;
            uassert(ErrorCodes::CommandFailed,
                    "No chunk found with the provided shard key bounds",
                    validBounds);

            ChunkType chunkType;
            chunkType.setCollectionUUID(cm.getUUID());
            chunkType.setMin(chunk.getMin());
            chunkType.setMax(chunk.getMax());
            chunkType.setShard(chunk.getShardId());
            chunkType.setVersion(cm.getVersion());

            {
                // TODO SERVER-64324 replace this scope calling moveRange instead of moveChunk
                MigrationSecondaryThrottleOptions secondaryThrottle = [&]() {
                    if (!req.getSecondaryThrottle()) {
                        return MigrationSecondaryThrottleOptions::create(
                            MigrationSecondaryThrottleOptions::kOff);
                    }

                    return MigrationSecondaryThrottleOptions::createWithWriteConcern(
                        opCtx->getWriteConcern());
                }();

                const bool forceJumbo = req.getForceJumbo() != ForceJumbo::kDoNotForce;
                uassertStatusOK(Balancer::get(opCtx)->moveSingleChunk(opCtx,
                                                                      nss,
                                                                      chunkType,
                                                                      req.getToShard(),
                                                                      secondaryThrottle,
                                                                      req.getWaitForDelete(),
                                                                      forceJumbo));
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };
} _cfgsvrMoveRange;

}  // namespace
}  // namespace mongo