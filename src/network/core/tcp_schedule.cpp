/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tcp_schedule.cpp Server part of the schedule API network protocol. */

#include "../../stdafx.h"
#include "../../debug.h"
#include "../../string_func.h"
#include "../../strings_func.h"
#include "../../settings_type.h"
#include "../../station_base.h"
#include "../../vehicle_base.h"
#include "../../waypoint_base.h"
#include "../../order_base.h"
#include "../../order_enums_to_json.h"
#include "../../order_serialisation.h"
#include "../../order_cmd.h"
#include "../../command_func.h"
#include "../../command_type.h"
#include "../../company_func.h"
#include "../../timetable.h"
#include "../../map_func.h"
#include "../../station_crossing.h"
#include "../../core/pool_func.hpp"
#include "tcp_schedule.h"

#include "table/strings.h"

#include "../../3rdparty/nlohmann/json.hpp"

#include "../../safeguards.h"

/** The pool with schedule API sockets. */
NetworkScheduleSocketPool _networkschedulesocket_pool("NetworkScheduleSocket");
INSTANTIATE_POOL_METHODS(NetworkScheduleSocket)

/** The global request/response queues. */
static ScheduleQueues _schedule_queues;

/** The global schedule API token. */
static std::string _schedule_api_token;

/** The listening socket(s) for the schedule API. */
static SocketList _schedule_listen_sockets;

/**
 * Initialise the schedule API token from the current setting.
 * Called from NetworkServerStart.
 */
void NetworkScheduleInitToken()
{
	_schedule_api_token = _settings_client.network.schedule_api_token;
}

/**
 * Create a new socket handler for a schedule API client.
 * @param index The index in the pool.
 * @param s The connected socket.
 */
ServerNetworkScheduleSocketHandler::ServerNetworkScheduleSocketHandler(ScheduleClientID index, SOCKET s) :
	PoolItemBase(index)
{
	this->sock = s;
	this->status = SCHEDULE_STATUS_INACTIVE;
	Debug(net, 3, "[schedule] Client connected (index {})", index);
}

/**
 * Clean up the socket handler.
 */
ServerNetworkScheduleSocketHandler::~ServerNetworkScheduleSocketHandler()
{
	Debug(net, 3, "[schedule] Client disconnected (index {})", this->index);
	this->CloseSocket();
}

/**
 * Close the underlying OS socket.
 */
void ServerNetworkScheduleSocketHandler::CloseSocket()
{
	if (this->sock != INVALID_SOCKET) closesocket(this->sock);
	this->sock = INVALID_SOCKET;
}

/**
 * Close the connection and delete this handler.
 */
void ServerNetworkScheduleSocketHandler::CloseConnection(bool error)
{
	Debug(net, 3, "[schedule] Closing connection (index {})", this->index);
	this->CloseSocket();
	delete this;
}

/**
 * Start listening on the given port.
 * @param port The port to listen on.
 * @return true if listening started successfully.
 */
/* static */ bool ServerNetworkScheduleSocketHandler::Listen(uint16_t port)
{
	assert(_schedule_listen_sockets.empty());

	NetworkAddressList addresses;
	GetBindAddresses(&addresses, port);

	for (NetworkAddress &address : addresses) {
		address.Listen(SOCK_STREAM, &_schedule_listen_sockets);
	}

	if (_schedule_listen_sockets.empty()) {
		Debug(net, 0, "Could not start schedule API listener on port {}", port);
		return false;
	}

	Debug(net, 3, "[schedule] Listening on port {}", port);
	return true;
}

/**
 * Close all listening sockets.
 */
/* static */ void ServerNetworkScheduleSocketHandler::CloseListeners()
{
	for (auto &s : _schedule_listen_sockets) {
		closesocket(s.first);
	}
	_schedule_listen_sockets.clear();
	Debug(net, 5, "[schedule] Closed listeners");
}

/**
 * Accept new connections on the listening socket.
 */
static void ScheduleAcceptClients()
{
	for (auto &s : _schedule_listen_sockets) {
		SOCKET ls = s.first;
		for (;;) {
			struct sockaddr_storage sin{};
			socklen_t sin_len = sizeof(sin);
			SOCKET client_sock = accept(ls, (struct sockaddr*)&sin, &sin_len);
			if (client_sock == INVALID_SOCKET) break;

			SetNonBlocking(client_sock);
			SetNoDelay(client_sock);

			NetworkAddress address(sin, sin_len);
			Debug(net, 3, "[schedule] Client connected from {} on frame {}", address.GetHostname(), _frame_counter);

			/* Check if we can allocate another client slot. */
			if (!ServerNetworkScheduleSocketHandler::CanAllocateItem()) {
				Debug(net, 2, "[schedule] Max clients reached, refusing connection from {}", address.GetHostname());
				closesocket(client_sock);
				continue;
			}

			ServerNetworkScheduleSocketHandler *handler = ServerNetworkScheduleSocketHandler::Create(client_sock);
			handler->address = address;
		}
	}
}

/**
 * Try to read one complete length-prefixed JSON frame from the socket.
 * @return The JSON string if a complete frame is available, else std::nullopt.
 */
std::optional<std::string> ServerNetworkScheduleSocketHandler::TryReadFrame()
{
	/* Phase 1: reading the 4-byte length header. */
	if (this->read_state.expected_length == 0) {
		uint8_t header[4];
		ssize_t n = recv(this->sock, reinterpret_cast<char *>(header), sizeof(header), MSG_PEEK);
		if (n <= 0) {
			/* No data available (wouldblock) or error/closed. */
			if (n == 0 || !NetworkError::GetLast().WouldBlock()) {
				this->CloseConnection();
			}
			return std::nullopt;
		}
		if (static_cast<size_t>(n) < sizeof(header)) {
			/* Not enough data yet for the full header. */
			return std::nullopt;
		}
		/* Consume the header. */
		recv(this->sock, reinterpret_cast<char *>(header), sizeof(header), 0);
		this->read_state.expected_length =
			(static_cast<uint32_t>(header[0]) << 24) |
			(static_cast<uint32_t>(header[1]) << 16) |
			(static_cast<uint32_t>(header[2]) << 8) |
			static_cast<uint32_t>(header[3]);

		if (this->read_state.expected_length > 1024 * 1024) { /* 1 MB sanity limit */
			Debug(net, 0, "[schedule] Frame too large ({} bytes), disconnecting", this->read_state.expected_length);
			this->CloseConnection();
			return std::nullopt;
		}

		this->read_state.buffer.clear();
		this->read_state.buffer.reserve(this->read_state.expected_length);
		this->read_state.received = 0;
	}

	/* Phase 2: reading the payload. */
	{
		uint32_t remaining = this->read_state.expected_length - this->read_state.received;
		size_t old_size = this->read_state.buffer.size();
		this->read_state.buffer.resize(old_size + remaining);

		ssize_t n = recv(this->sock, reinterpret_cast<char *>(this->read_state.buffer.data() + old_size), remaining, 0);
		if (n <= 0) {
			this->read_state.buffer.resize(old_size);
			if (n == 0 || !NetworkError::GetLast().WouldBlock()) {
				this->CloseConnection();
			}
			return std::nullopt;
		}

		this->read_state.received += static_cast<uint32_t>(n);
		this->read_state.buffer.resize(old_size + n);

		if (this->read_state.received < this->read_state.expected_length) {
			return std::nullopt;
		}
	}

	/* Complete frame received. */
	std::string json_str(reinterpret_cast<const char *>(this->read_state.buffer.data()), this->read_state.received);
	this->read_state.expected_length = 0;
	this->read_state.received = 0;
	this->read_state.buffer.clear();
	this->read_state.buffer.shrink_to_fit();
	return json_str;
}

/**
 * Send a raw JSON string as a length-prefixed frame.
 */
bool ServerNetworkScheduleSocketHandler::SendRawJSON(std::string_view json)
{
	if (this->sock == INVALID_SOCKET) return false;
	if (json.size() > 1024 * 1024) {
		Debug(net, 0, "[schedule] Response too large ({} bytes), dropping", json.size());
		return false;
	}

	uint32_t len = static_cast<uint32_t>(json.size());
	uint8_t header[4] = {
		static_cast<uint8_t>((len >> 24) & 0xFF),
		static_cast<uint8_t>((len >> 16) & 0xFF),
		static_cast<uint8_t>((len >> 8) & 0xFF),
		static_cast<uint8_t>(len & 0xFF),
	};

	/* Send header. */
	ssize_t sent = send(this->sock, reinterpret_cast<const char *>(header), sizeof(header), 0);
	if (sent < 0) {
		if (!NetworkError::GetLast().WouldBlock()) {
			this->CloseConnection();
		}
		return false;
	}

	/* Send payload in a loop (non-blocking socket may not send all at once). */
	const char *payload = json.data();
	size_t remaining = json.size();
	while (remaining > 0) {
		sent = send(this->sock, payload, remaining, 0);
		if (sent < 0) {
			if (!NetworkError::GetLast().WouldBlock()) {
				this->CloseConnection();
				return false;
			}
			/* Would block; can't send more now. The remaining data is lost
			 * in this simple implementation (no retry queue for raw sends). */
			return false;
		}
		payload += sent;
		remaining -= sent;
	}

	return true;
}

/**
 * Enqueue a response to be sent to a client.
 * Thread-safe; may be called from the main thread.
 */
/* static */ void ServerNetworkScheduleSocketHandler::EnqueueResponse(ServerNetworkScheduleSocketHandler *client, std::string &&json)
{
	std::lock_guard<std::mutex> lock(_schedule_queues.response_mutex);
	_schedule_queues.responses.push_back({ std::move(json), client });
}

/**
 * Handle a single JSON request on the network thread.
 * Ping and Auth are handled immediately; everything else is enqueued
 * for the main game thread.
 */
void ServerNetworkScheduleSocketHandler::HandleJSONRequest(const std::string &json_str)
{
	/* Minimal hand-parsed JSON to extract "type" and "id" fields.
	 * Full parsing with nlohmann::json is done on the main thread. */

	/* Find the "type" field. */
	auto type_pos = json_str.find("\"type\"");
	if (type_pos == std::string::npos) {
		this->SendRawJSON("{\"status\":\"error\",\"error\":\"missing type field\"}");
		return;
	}
	auto value_start = json_str.find('"', type_pos + 6);
	if (value_start == std::string::npos) {
		this->SendRawJSON("{\"status\":\"error\",\"error\":\"malformed type field\"}");
		return;
	}
	auto value_end = json_str.find('"', value_start + 1);
	if (value_end == std::string::npos) {
		this->SendRawJSON("{\"status\":\"error\",\"error\":\"malformed type field\"}");
		return;
	}
	std::string msg_type = json_str.substr(value_start + 1, value_end - value_start - 1);

	/* Extract "id" if present. */
	std::string req_id;
	auto id_pos = json_str.find("\"id\"");
	if (id_pos != std::string::npos) {
		auto id_start = json_str.find('"', id_pos + 4);
		if (id_start != std::string::npos) {
			auto id_end = json_str.find('"', id_start + 1);
			if (id_end != std::string::npos) {
				req_id = json_str.substr(id_start + 1, id_end - id_start - 1);
			}
		}
	}

	/* Helper: build a JSON response with request id echoed back. */
	auto make_response = [&](std::string_view status, std::string_view error_msg, std::string_view body_json) -> std::string {
		std::string resp = "{";
		if (!req_id.empty()) {
			resp += "\"id\":\"" + req_id + "\",";
		}
		resp += "\"status\":\"" + std::string(status) + "\"";
		if (!error_msg.empty()) {
			resp += ",\"error\":\"" + std::string(error_msg) + "\"";
		}
		if (!body_json.empty()) {
			resp += ",\"body\":" + std::string(body_json);
		}
		resp += "}";
		return resp;
	};

	if (msg_type == "Ping") {
		this->SendRawJSON(make_response("ok", "", "{}"));
		return;
	}

	if (msg_type == "Auth") {
		/* Extract the token field. */
		auto token_pos = json_str.find("\"token\"");
		if (token_pos == std::string::npos) {
			this->SendRawJSON(make_response("error", "missing token", ""));
			return;
		}
		auto token_start = json_str.find('"', token_pos + 7);
		if (token_start == std::string::npos) {
			this->SendRawJSON(make_response("error", "malformed token", ""));
			return;
		}
		auto token_end = json_str.find('"', token_start + 1);
		if (token_end == std::string::npos) {
			this->SendRawJSON(make_response("error", "malformed token", ""));
			return;
		}
		std::string token = json_str.substr(token_start + 1, token_end - token_start - 1);

		bool authenticated = false;
		if (_schedule_api_token.empty()) {
			authenticated = true; /* No token configured → accept all. */
		} else if (token == _schedule_api_token) {
			authenticated = true;
		}

		if (authenticated) {
			this->status = SCHEDULE_STATUS_ACTIVE;
			this->SendRawJSON(make_response("ok", "", R"({"authenticated":true})"));
			Debug(net, 3, "[schedule] Client authenticated (index {})", this->index);
		} else {
			this->SendRawJSON(make_response("error", "invalid token", ""));
			Debug(net, 2, "[schedule] Auth failed for client (index {})", this->index);
		}
		return;
	}

	/* All other message types require authentication. */
	if (this->status != SCHEDULE_STATUS_ACTIVE) {
		this->SendRawJSON(make_response("error", "not authenticated", ""));
		return;
	}

	/* Enqueue for the main thread. */
	{
		std::lock_guard<std::mutex> lock(_schedule_queues.request_mutex);
		_schedule_queues.requests.push_back({ std::string(json_str), this });
	}
}

/**
 * Send pending data for all schedule clients (called from network thread).
 */
/* static */ void ServerNetworkScheduleSocketHandler::Send()
{
	/* Drain the response queue. */
	{
		std::lock_guard<std::mutex> lock(_schedule_queues.response_mutex);
		while (!_schedule_queues.responses.empty()) {
			auto resp = std::move(_schedule_queues.responses.front());
			_schedule_queues.responses.pop_front();
			if (resp.client != nullptr && resp.client->IsConnected()) {
				resp.client->SendRawJSON(resp.json);
			}
		}
	}
}

/**
 * Receive data from all schedule clients (called from network thread).
 * Handles both accepting new connections and reading frames.
 */
/* static */ bool ServerNetworkScheduleSocketHandler::Receive()
{
	fd_set read_fd, write_fd;
	struct timeval tv;
	FD_ZERO(&read_fd);
	FD_ZERO(&write_fd);

	SOCKET max_sock = INVALID_SOCKET;

	/* Add listening sockets. */
	for (auto &s : _schedule_listen_sockets) {
		SOCKET ls = s.first;
		FD_SET(ls, &read_fd);
		if (ls > max_sock) max_sock = ls;
	}

	/* Add connected sockets. */
	for (auto *handler : ServerNetworkScheduleSocketHandler::Iterate()) {
		SOCKET cs = handler->sock;
		if (cs == INVALID_SOCKET) continue;
		FD_SET(cs, &read_fd);
		FD_SET(cs, &write_fd);
		if (cs > max_sock) max_sock = cs;
	}

	if (max_sock == INVALID_SOCKET) return true;

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (select(FD_SETSIZE, &read_fd, &write_fd, nullptr, &tv) < 0) return false;

	/* Accept new clients. */
	ScheduleAcceptClients();

	/* Read from connected clients. */
	for (auto *handler : ServerNetworkScheduleSocketHandler::Iterate()) {
		if (handler->sock == INVALID_SOCKET) continue;

		handler->writable = !!FD_ISSET(handler->sock, &write_fd);

		if (FD_ISSET(handler->sock, &read_fd)) {
			/* Read all available frames. */
			while (true) {
				auto json = handler->TryReadFrame();
				if (!json.has_value()) break;
				handler->HandleJSONRequest(json.value());
			}
		}
	}

	return _networking;
}

/**
 * Build a simple JSON response with optional id, status, error, and body.
 */
static std::string BuildJSONResponse(const std::string &req_id, std::string_view status, std::string_view error_msg, std::string_view body_json)
{
	std::string resp = "{";
	if (!req_id.empty()) {
		resp += "\"id\":\"" + req_id + "\",";
	}
	resp += "\"status\":\"" + std::string(status) + "\"";
	if (!error_msg.empty()) {
		resp += ",\"error\":\"" + std::string(error_msg) + "\"";
	}
	if (!body_json.empty()) {
		resp += ",\"body\":" + std::string(body_json);
	}
	resp += "}";
	return resp;
}

/**
 * Process pending schedule API requests on the main game thread.
 * This has full access to game state and can call DoCommandP etc.
 */
/* static */ bool ServerNetworkScheduleSocketHandler::HasActiveClients()
{
	for (auto *handler : ServerNetworkScheduleSocketHandler::Iterate()) {
		if (handler->status == SCHEDULE_STATUS_ACTIVE) return true;
	}
	return false;
}

/* static */ void ServerNetworkScheduleSocketHandler::ProcessRequests()
{
	/* Snapshot the request queue. */
	std::deque<ScheduleRequest> local_requests;
	{
		std::lock_guard<std::mutex> lock(_schedule_queues.request_mutex);
		local_requests.swap(_schedule_queues.requests);
	}

	for (auto &req : local_requests) {
		/* Parse JSON with nlohmann::json. */
		nlohmann::json j;
		try {
			j = nlohmann::json::parse(req.json);
		} catch (const nlohmann::json::parse_error &) {
			auto resp = BuildJSONResponse("", "error", "invalid JSON", "");
			ServerNetworkScheduleSocketHandler::EnqueueResponse(req.client, std::move(resp));
			continue;
		}

		std::string req_id = j.value("id", std::string());
		std::string msg_type = j.value("type", std::string());

		auto send_ok = [&](std::string_view body_json) {
			auto resp = BuildJSONResponse(req_id, "ok", "", body_json);
			ServerNetworkScheduleSocketHandler::EnqueueResponse(req.client, std::move(resp));
		};
		auto send_error = [&](std::string_view error_msg) {
			auto resp = BuildJSONResponse(req_id, "error", error_msg, "");
			ServerNetworkScheduleSocketHandler::EnqueueResponse(req.client, std::move(resp));
		};

		if (msg_type == "ListStations") {
			nlohmann::json stations = nlohmann::json::array();
			for (const Station *st : Station::Iterate()) {
				if (!st->facilities.Test(StationFacility::Train)) continue;
				nlohmann::json obj;
				obj["id"] = st->index.base();
				obj["name"] = GetString(STR_STATION_NAME, st->index.base());
				obj["x"] = TileX(st->xy);
				obj["y"] = TileY(st->xy);
				obj["train"] = true;
				stations.push_back(std::move(obj));
			}
			nlohmann::json body;
			body["stations"] = std::move(stations);
			send_ok(body.dump());
			continue;
		}

		if (msg_type == "ListWaypoints") {
			nlohmann::json waypoints = nlohmann::json::array();
			for (const Waypoint *wp : Waypoint::Iterate()) {
				if (HasBit(wp->waypoint_flags, WPF_ROAD)) continue; /* Only rail waypoints */
				nlohmann::json obj;
				obj["id"] = wp->index.base();
				obj["name"] = GetString(STR_WAYPOINT_NAME, wp->index.base());
				obj["x"] = TileX(wp->xy);
				obj["y"] = TileY(wp->xy);
				waypoints.push_back(std::move(obj));
			}
			nlohmann::json body;
			body["waypoints"] = std::move(waypoints);
			send_ok(body.dump());
			continue;
		}

		if (msg_type == "ListVehicles") {
			nlohmann::json vehicles = nlohmann::json::array();
			for (const Vehicle *v : Vehicle::Iterate()) {
				if (v->type != VehicleType::Train || !v->IsPrimaryVehicle()) continue;
				nlohmann::json obj;
				obj["vehicle_id"] = v->index.base();
				obj["company"] = v->owner.base();
				obj["name"] = GetString(STR_VEHICLE_NAME, v->index.base());
				obj["type"] = "train";
				obj["current_tile_x"] = TileX(v->tile);
				obj["current_tile_y"] = TileY(v->tile);
				vehicles.push_back(std::move(obj));
			}
			nlohmann::json body;
			body["vehicles"] = std::move(vehicles);
			send_ok(body.dump());
			continue;
		}

		if (msg_type == "GetTimetable") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };
				const Vehicle *v = Vehicle::GetIfValid(veh_id);
				if (v == nullptr || v->type != VehicleType::Train || !v->IsPrimaryVehicle()) {
					send_error("vehicle not found");
					continue;
				}
				const OrderList *ol = v->orders;
				if (ol == nullptr) {
					send_error("no orders");
					continue;
				}
				std::string timetable_json = OrderListToJSONString(ol);
				nlohmann::json resp_body;
				try {
					resp_body["timetable"] = nlohmann::json::parse(timetable_json);
				} catch (...) {
					resp_body["timetable_raw"] = timetable_json;
				}
				send_ok(resp_body.dump());
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid vehicle_id");
			}
			continue;
		}

		if (msg_type == "GetDispatchSchedules") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };
				const Vehicle *v = Vehicle::GetIfValid(veh_id);
				if (v == nullptr || v->type != VehicleType::Train || !v->IsPrimaryVehicle()) {
					send_error("vehicle not found");
					continue;
				}
				const OrderList *ol = v->orders;
				if (ol == nullptr) {
					send_error("no orders");
					continue;
				}
				nlohmann::ordered_json schedules = nlohmann::ordered_json::array();
				const auto &sd_data = ol->GetScheduledDispatchScheduleSet();
				for (const auto &sd : sd_data) {
					schedules.push_back(DispatchScheduleToJSON(sd));
				}
				nlohmann::json resp_body;
				resp_body["schedules"] = std::move(schedules);
				send_ok(resp_body.dump());
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid vehicle_id");
			}
			continue;
		}

		if (msg_type == "GetStationCrossings") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };

				/* Reference tick for computing offsets: prefer the first dispatch
				 * schedule's start tick, else the vehicle's timetable_start. */
				Vehicle *v = Vehicle::GetIfValid(veh_id);
				StateTicks ref = StateTicks{0};
				if (v != nullptr) {
					if (v->orders != nullptr) {
						const auto &scheds = v->orders->GetScheduledDispatchScheduleSet();
						if (!scheds.empty()) ref = scheds.front().GetScheduledDispatchStartTick();
						else if (v->timetable_start != StateTicks{0}) ref = v->timetable_start;
					} else if (v->timetable_start != StateTicks{0}) {
						ref = v->timetable_start;
					}
				}

				auto records = StationCrossingTracker::GetVehicleCrossings(veh_id);

				nlohmann::json crossings = nlohmann::json::array();
				for (auto &tup : records) {
					VehicleOrderID order_idx = std::get<0>(tup);
					StationID st_id = std::get<1>(tup);
					const StationCrossingEntry &e = std::get<2>(tup);
					nlohmann::json o;
					o["order_index"] = order_idx;
					o["station_id"] = st_id.base();
					o["tick"] = e.tick.base();
					o["travel_time"] = static_cast<int64_t>(e.travel_time);
					o["tile_x"] = TileX(e.tile);
					o["tile_y"] = TileY(e.tile);
					crossings.push_back(std::move(o));
				}

				nlohmann::json resp_body;
				resp_body["vehicle_id"] = veh_id.base();
				resp_body["reference_tick"] = ref.base();
				resp_body["crossings"] = std::move(crossings);
				send_ok(resp_body.dump());
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid vehicle_id");
			}
			continue;
		}

		if (msg_type == "DeleteOrder") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				int order_idx = body.at("order_index").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };
				VehicleOrderID order_index{ static_cast<uint16_t>(order_idx) };
				Vehicle *v = Vehicle::GetIfValid(veh_id);
				if (v == nullptr || v->type != VehicleType::Train || !v->IsPrimaryVehicle()) {
					send_error("vehicle not found");
					continue;
				}
				if (v->orders == nullptr || order_index >= v->GetNumOrders()) {
					send_error("invalid order index");
					continue;
				}
				CompanyID old_company = _current_company;
				_current_company = v->owner;
				bool ok = Command<Commands::DeleteOrder>::Post(STR_EMPTY, TileIndex{0}, veh_id, order_index);
				_current_company = old_company;
				if (ok) {
					send_ok("{}");
				} else {
					send_error("delete order failed");
				}
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid vehicle_id or order_index");
			}
			continue;
		}

		if (msg_type == "InsertOrder") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };
				Vehicle *v = Vehicle::GetIfValid(veh_id);
				if (v == nullptr || v->type != VehicleType::Train || !v->IsPrimaryVehicle()) {
					send_error("vehicle not found");
					continue;
				}
				/* order_index: -1 = append, otherwise insert at that position */
				int order_idx_int = body.value("order_index", -1);
				VehicleOrderID insert_idx = (order_idx_int < 0) ? INVALID_VEH_ORDER_ID : static_cast<VehicleOrderID>(order_idx_int);
				auto &order_json = body.at("order");

				/* Build a minimal timetable JSON: { "vehicle-type": "train", "orders": [ order ] } */
				nlohmann::json wrapper;
				wrapper["vehicle-type"] = VehicleType::Train;
				wrapper["orders"] = nlohmann::json::array();
				wrapper["orders"].push_back(order_json);
				std::string json_str = wrapper.dump();

				CompanyID old_company = _current_company;
				_current_company = v->owner;
				OrderImportErrors errors = ImportJsonOrderList(v, json_str, insert_idx, false, true);
				_current_company = old_company;

				nlohmann::json resp_body;
				resp_body["has_errors"] = errors.HasErrors();
				if (errors.HasErrors()) {
					nlohmann::json global_errs = nlohmann::json::array();
					for (auto &e : errors.global) {
						nlohmann::json err_obj;
						err_obj["msg"] = e.msg;
						err_obj["type"] = static_cast<int>(e.type);
						global_errs.push_back(std::move(err_obj));
					}
					resp_body["global_errors"] = std::move(global_errs);
				}
				send_ok(resp_body.dump());
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid vehicle_id or order");
			}
			continue;
		}

		if (msg_type == "SetOrder") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				int order_idx = body.at("order_index").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };
				VehicleOrderID order_index{ static_cast<uint16_t>(order_idx) };
				Vehicle *v = Vehicle::GetIfValid(veh_id);
				if (v == nullptr || v->type != VehicleType::Train || !v->IsPrimaryVehicle()) {
					send_error("vehicle not found");
					continue;
				}
				if (v->orders == nullptr || order_index >= v->GetNumOrders()) {
					send_error("invalid order index");
					continue;
				}
				auto &order_json = body.at("order");

				CompanyID old_company = _current_company;
				_current_company = v->owner;

				bool deleted = Command<Commands::DeleteOrder>::Post(STR_EMPTY, TileIndex{0}, veh_id, order_index);
				if (!deleted) {
					_current_company = old_company;
					send_error("delete order failed");
					continue;
				}

				nlohmann::json wrapper;
				wrapper["vehicle-type"] = VehicleType::Train;
				wrapper["orders"] = nlohmann::json::array();
				wrapper["orders"].push_back(order_json);
				std::string json_str = wrapper.dump();

				OrderImportErrors errors = ImportJsonOrderList(v, json_str, order_index, false, true);
				_current_company = old_company;

				nlohmann::json resp_body;
				resp_body["has_errors"] = errors.HasErrors();
				if (errors.HasErrors()) {
					nlohmann::json global_errs = nlohmann::json::array();
					for (auto &e : errors.global) {
						nlohmann::json err_obj;
						err_obj["msg"] = e.msg;
						err_obj["type"] = static_cast<int>(e.type);
						global_errs.push_back(std::move(err_obj));
					}
					resp_body["global_errors"] = std::move(global_errs);
				}
				send_ok(resp_body.dump());
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid vehicle_id or order");
			}
			continue;
		}

		if (msg_type == "SetFullDispatch") {
			try {
				auto &body = j.at("body");
				int veh_id_int = body.at("vehicle_id").get<int>();
				VehicleID veh_id{ static_cast<uint16_t>(veh_id_int) };
				Vehicle *v = Vehicle::GetIfValid(veh_id);
				if (v == nullptr || v->type != VehicleType::Train || !v->IsPrimaryVehicle()) {
					send_error("vehicle not found");
					continue;
				}

				CompanyID old_company = _current_company;
				_current_company = v->owner;

				/* Ensure the vehicle has an order list and enable scheduled dispatch. */
				if (v->orders == nullptr) {
					v->orders = OrderList::Create(nullptr, v);
				}
				v->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);

				/* Replace the dispatch schedule set. */
				auto &scheds = v->orders->GetScheduledDispatchScheduleSet();
				scheds.clear();

				auto &schedules_json = body.at("schedules");
				for (auto &sched_json : schedules_json) {
					scheds.emplace_back();
					DispatchSchedule &ds = scheds.back();

					/* Set duration. */
					uint32_t duration = sched_json.value("duration", 0u);
					ds.SetScheduledDispatchDuration(duration);

					/* Set start tick. */
					if (sched_json.contains("absolute-start-time")) {
						ds.SetScheduledDispatchStartTick(StateTicks{ sched_json.at("absolute-start-time").get<int32_t>() });
					} else if (sched_json.contains("relative-start-time")) {
						/* Compute absolute start from relative. For simplicity, use 0 as base. */
						ds.SetScheduledDispatchStartTick(StateTicks{ sched_json.at("relative-start-time").get<int32_t>() });
					} else {
						ds.SetScheduledDispatchStartTick(StateTicks{-1});
					}

					/* Set max delay. */
					if (sched_json.contains("max-delay")) {
						ds.SetScheduledDispatchDelay(sched_json.at("max-delay").get<int32_t>());
					}

					/* Add slots. */
					auto &slots_json = sched_json.at("slots");
					for (auto &slot_json : slots_json) {
						uint32_t offset = 0;
						uint16_t slot_flags = 0;
						DispatchSlotRouteID route_id = 0;

						if (slot_json.is_number()) {
							offset = slot_json.get<uint32_t>();
						} else if (slot_json.is_object()) {
							offset = slot_json.value("offset", 0u);
							slot_flags = slot_json.value("flags", static_cast<uint16_t>(0));
							route_id = slot_json.value("route", static_cast<DispatchSlotRouteID>(0));
						}

						ds.AddScheduledDispatch(offset, slot_flags, route_id);
					}

					ds.ResortDispatchOffsets();
					ds.UpdateScheduledDispatch(nullptr);
				}

				SetTimetableWindowsDirty(v, STWDF_SCHEDULED_DISPATCH);
				_current_company = old_company;

				nlohmann::json resp_body;
				resp_body["schedule_count"] = scheds.size();
				send_ok(resp_body.dump());
			} catch (const nlohmann::json::exception &) {
				send_error("missing or invalid body");
			}
			continue;
		}

		/* Unknown type. */
		send_error("unknown type: " + msg_type);
	}
}
