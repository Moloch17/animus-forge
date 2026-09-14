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

#ifndef ANIMUS_LOCKSTEP_SERVER_H
#define ANIMUS_LOCKSTEP_SERVER_H

#include "Protocol.h"
#include <cstddef>
#include <string>
#include <vector>

namespace Animus
{
    /// One outgoing buffer of a message payload.
    struct Chunk
    {
        void const* Data;
        std::size_t Size;
    };

    /// Single-client Unix domain socket server, driven synchronously from the world thread.
    ///
    /// Plain POSIX rather than Boost.Asio: the sim host is Linux-only, everything is blocking
    /// lock-step, and every wait has to poll World::IsStopped() so SIGINT can still end the process
    /// while the world thread sits waiting on the learner.
    class LockstepServer
    {
    public:
        LockstepServer() = default;
        ~LockstepServer();

        LockstepServer(LockstepServer const&) = delete;
        LockstepServer& operator=(LockstepServer const&) = delete;

        bool Listen(std::string const& path);
        void Shutdown();

        /// Block until a client connects and sends a valid HELLO, or the world stops.
        bool AcceptClient();
        void DropClient();
        [[nodiscard]] bool HasClient() const { return _client >= 0; }

        /// Send one message; the payload is the concatenation of `chunks`.
        bool Send(MsgType type, std::vector<Chunk> const& chunks);

        /// Receive one message into `dst`, which must be exactly the expected payload size.
        /// `type` is the type actually received. On CLOSE (no payload) the client is dropped and
        /// true is returned. False on error, disconnect or world stop.
        bool Receive(MsgType& type, void* dst, std::size_t size);

    private:
        bool WaitReadable(int fd);
        bool ReadExact(void* dst, std::size_t size);

        std::string _path;
        int _listener = -1;
        int _client = -1;
    };
}

#endif
