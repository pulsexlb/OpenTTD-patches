/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tcp_schedule.h Basic functions to receive JSON length-prefixed messages for the schedule API. */

#ifndef NETWORK_CORE_TCP_SCHEDULE_H
#define NETWORK_CORE_TCP_SCHEDULE_H

#include "os_abstraction.h"
#include "tcp.h"
#include "../network_internal.h"
#include "../../core/pool_type.hpp"
#include "../../core/pool_id_type.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** Maximum number of connected schedule API clients. */
static const uint8_t MAX_SCHEDULE_CLIENTS = 8;

/** Unique ID for schedule API client connections. */
struct ScheduleClientIDTag : public PoolIDTraits<uint8_t, MAX_SCHEDULE_CLIENTS, 0xFF> {};
using ScheduleClientID = PoolID<ScheduleClientIDTag>;

/** Status of a schedule API client connection. */
enum ScheduleClientStatus : uint8_t {
	SCHEDULE_STATUS_INACTIVE,  ///< Client just connected, waiting for Auth.
	SCHEDULE_STATUS_ACTIVE,    ///< Client is authenticated and active.
};

/**
 * Forward declaration.
 */
class ServerNetworkScheduleSocketHandler;

/**
 * Pool type for schedule API connections.
 */
using NetworkScheduleSocketPool = Pool<ServerNetworkScheduleSocketHandler, ScheduleClientID, 2, PoolType::NetworkSchedule>;
extern NetworkScheduleSocketPool _networkschedulesocket_pool;

/**
 * A pending request from a schedule API client, received on the network thread
 * and processed on the main game thread.
 */
struct ScheduleRequest {
	std::string json;                              ///< The raw JSON payload.
	ServerNetworkScheduleSocketHandler *client;     ///< The client connection that sent this.
};

/**
 * A pending response to be sent to a schedule API client, produced on the main
 * game thread and delivered on the network thread.
 */
struct ScheduleResponse {
	std::string json;                              ///< The JSON response payload.
	ServerNetworkScheduleSocketHandler *client;     ///< The target client connection.
};

/**
 * Thread-safe queues for requests/responses between network thread and main thread.
 */
struct ScheduleQueues {
	std::mutex request_mutex;
	std::deque<ScheduleRequest> requests;

	std::mutex response_mutex;
	std::deque<ScheduleResponse> responses;
};

/**
 * Initialise the schedule API token from the current setting.
 * Called from NetworkServerStart.
 */
void NetworkScheduleInitToken();

/**
 * Main socket handler for schedule API connections.
 *
 * Protocol: length-prefixed JSON.
 * - First 4 bytes: big-endian uint32 payload length (excluding the 4-byte header).
 * - Followed by exactly that many bytes of UTF-8 JSON.
 *
 * The JSON envelope format:
 *   Request:  { "id": "...", "type": "...", "token": "...", "body": {...} }
 *   Response: { "id": "...", "status": "ok"|"error", "error": "...", "body": {...} }
 *
 * This handler manages its own I/O directly (raw recv/send) instead of using
 * the OpenTTD Packet system, because the schedule API uses a JSON-over-TCP
 * protocol rather than the binary Packet format.
 */
class ServerNetworkScheduleSocketHandler : public NetworkScheduleSocketPool::PoolItem<&_networkschedulesocket_pool> {
public:
	ScheduleClientStatus status = SCHEDULE_STATUS_INACTIVE; ///< Auth state.
	NetworkAddress address;                                ///< Address of the client.
	SOCKET sock = INVALID_SOCKET;                          ///< The connected socket.
	bool writable = false;                                 ///< Whether the socket is writable.

	/**
	 * Create a new schedule socket handler.
	 * @param index The index in the pool.
	 * @param s The connected socket.
	 */
	ServerNetworkScheduleSocketHandler(ScheduleClientID index, SOCKET s);

	~ServerNetworkScheduleSocketHandler();

	/**
	 * Check if this socket is still connected.
	 */
	bool IsConnected() const { return this->sock != INVALID_SOCKET; }

	/**
	 * Close the underlying OS socket.
	 */
	void CloseSocket();

	/**
	 * Close the connection and delete this handler.
	 */
	void CloseConnection(bool error = true);

	/* ---- Static API called from network.cpp ---- */

	/**
	 * Start listening on the given port.
	 * @param port The port to listen on.
	 * @return true if listening started successfully.
	 */
	static bool Listen(uint16_t port);

	/**
	 * Close all listening sockets.
	 */
	static void CloseListeners();

	/**
	 * Send pending data for all connected schedule clients.
	 */
	static void Send();

	/**
	 * Receive data from all connected schedule clients (accept new, read frames).
	 * @return true if networking should continue.
	 */
	static bool Receive();

	/**
	 * Process all pending requests on the main game thread.
	 * Called from NetworkGameLoop (server side only).
	 */
	static void ProcessRequests();

	/**
	 * Check if any schedule API client is currently authenticated and active.
	 * @return true if at least one active client exists.
	 */
	static bool HasActiveClients();

	/**
	 * Enqueue a JSON response to be sent back to the client.
	 * Thread-safe: can be called from the main thread.
	 */
	static void EnqueueResponse(ServerNetworkScheduleSocketHandler *client, std::string &&json);

	/**
	 * Get the display name for debug logs.
	 */
	static std::string_view GetName() { return "schedule"; }

private:
	/** Per-connection state for partially-received frames. */
	struct ReadState {
		uint32_t expected_length = 0; ///< 0 = reading 4-byte header, >0 = reading payload.
		uint32_t received = 0;        ///< Bytes received so far in current frame.
		std::vector<uint8_t> buffer;  ///< Accumulated frame data.
	};
	ReadState read_state;

	/** Try to read one complete JSON frame. Returns the JSON string if ready. */
	std::optional<std::string> TryReadFrame();

	/** Send a raw JSON response (length-prefixed). */
	bool SendRawJSON(std::string_view json);

	/** Handle a single JSON request (network thread). */
	void HandleJSONRequest(const std::string &json_str);
};

#endif /* NETWORK_CORE_TCP_SCHEDULE_H */
