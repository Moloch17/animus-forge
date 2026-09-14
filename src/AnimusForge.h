/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_FORGE_H
#define ANIMUS_FORGE_H

#include "AnimusConfig.h"
#include "EnvPool.h"
#include "LockstepServer.h"
#include "Scenario.h"
#include <memory>

namespace Animus
{
    /// Module root: owns the scenario, the env pool and the learner connection, and runs one
    /// decision step every AnimusForge.DecisionTicks world ticks.
    class Forge
    {
    public:
        static Forge* Instance();

        void OnStartup();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// The running pool, or nullptr when the module is disabled, failed to start or has shut
        /// down. Set and cleared on the world thread while no map is updating, so map-thread hooks
        /// may read it without a lock.
        [[nodiscard]] EnvPool* ActivePool() const { return _running ? _pool.get() : nullptr; }

    private:
        Forge() = default;

        bool Start();
        void Fail(char const* reason);

        void LocalDecision();
        void RemoteDecision();
        bool SendSpec();
        bool SendStep();

        ForgeConfig _config;
        std::unique_ptr<Scenario> _scenario;
        std::unique_ptr<EnvPool> _pool;
        LockstepServer _server;

        bool _running = false;
        uint64 _ticks = 0;
        uint64 _decisions = 0;
        uint32 _tickMs = 0;
    };
}

#define sAnimusForge Animus::Forge::Instance()

#endif
