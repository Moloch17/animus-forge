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

#ifndef MOD_ANIMUS_FORGE_LEARNER_PROCESS_H
#define MOD_ANIMUS_FORGE_LEARNER_PROCESS_H

#include <chrono>
#include <string>
#include <sys/types.h>

namespace AnimusForge
{
    struct ForgeConfig;

    /// The Python learner as a child process of the worldserver.
    ///
    /// Lifetime is tied to the learner socket rather than to signals: when the server shuts down
    /// (or crashes) the socket closes, the learner's train loop sees the disconnect, saves
    /// latest.pt and exits by itself. Stop() only escalates if it does not.
    class LearnerProcess
    {
    public:
        LearnerProcess() = default;
        ~LearnerProcess();

        LearnerProcess(LearnerProcess const&) = delete;
        LearnerProcess& operator=(LearnerProcess const&) = delete;

        /// Validate the setup and spawn the learner. Returns false (with the reason logged) if the
        /// working directory or config is missing or the process cannot be started.
        bool Start(ForgeConfig const& config);

        /// Reap the child if it has exited, logging how it ended once. Cheap; safe to call often.
        void Poll();

        [[nodiscard]] bool IsRunning() const { return _pid > 0; }

        /// The last started learner has exited with status 0: its run reached total_env_steps.
        [[nodiscard]] bool FinishedCleanly() const { return _pid <= 0 && _exitedCleanly; }

        /// Wait up to `grace` for a voluntary exit, then SIGINT (the learner saves a checkpoint on
        /// KeyboardInterrupt), then SIGKILL.
        void Stop(std::chrono::milliseconds grace);

    private:
        bool WaitForExit(std::chrono::milliseconds timeout);
        void ReportExit(int status);

        pid_t _pid = -1;
        bool _exitedCleanly = false;
        std::string _logFile;
    };
}

#endif
