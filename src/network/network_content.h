/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file network_content.h Part of the network protocol handling content distribution. */

#ifndef NETWORK_CONTENT_H
#define NETWORK_CONTENT_H

#include <ranges>
#include "core/tcp_content.h"
#include "core/http.h"
#include "../core/container_func.hpp"
#include "../3rdparty/cpp-btree/btree_map.h"
#include <vector>

/** Vector with content info */
using ContentVector = std::vector<std::unique_ptr<ContentInfo>>;
/** Vector with constant content info */
using ConstContentVector = std::vector<const ContentInfo *>;

/** Callbacks for notifying others about incoming data */
struct ContentCallback {
	/**
	 * Callback for when the connection has finished
	 * @param success whether the connection was made or that we failed to make it
	 */
	virtual void OnConnect([[maybe_unused]] bool success) {}

	/**
	 * Callback for when the connection got disconnected.
	 */
	virtual void OnDisconnect() {}

	/**
	 * We received a content info.
	 * @param ci the content info
	 */
	virtual void OnReceiveContentInfo([[maybe_unused]] const ContentInfo &ci) {}

	/**
	 * We have progress in the download of a file
	 * @param ci the content info of the file
	 * @param bytes the number of bytes downloaded since the previous call
	 */
	virtual void OnDownloadProgress([[maybe_unused]] const ContentInfo &ci, [[maybe_unused]] int bytes) {}

	/**
	 * We have finished downloading a file
	 * @param cid the ContentID of the downloaded file
	 */
	virtual void OnDownloadComplete([[maybe_unused]] ContentID cid) {}

	/** Silentium */
	virtual ~ContentCallback() = default;
};

/** Download context for a single content file being downloaded over HTTP. */
class ContentDownload;
class ContentManifestDownload;

/**
 * Socket handler for the content server connection
 */
class ClientNetworkContentSocketHandler : public NetworkContentSocketHandler, ContentCallback {
protected:
	using ContentIDList = std::vector<ContentID>; ///< List of content IDs to (possibly) select.
	std::vector<ContentCallback *> callbacks;     ///< Callbacks to notify "the world"
	ContentIDList requested;                      ///< ContentIDs we already requested (so we don't do it again)
	ContentIDList queued;                         ///< ContentID queue to be requested.
	ContentVector infos;                          ///< All content info we received
	btree::btree_multimap<ContentID, ContentID> reverse_dependency_map; ///< Content reverse dependency map
	std::shared_ptr<ContentManifestDownload> manifest_download; ///< Current batch's independently buffered HTTP manifest.
	std::shared_ptr<TCPConnecter> connecter;      ///< Pending content-server connection; cancelled with the batch.

	std::vector<std::pair<std::unique_ptr<ContentInfo>, std::string>> pending_downloads; ///< Files parsed from the manifest, waiting to be downloaded
	std::vector<std::shared_ptr<ContentDownload>> active_downloads; ///< Files that are currently being downloaded
	uint failed_downloads = 0;                    ///< Number of failed downloads in the current batch
	uint32_t download_batch = 0;                  ///< Identifier of the current download batch; used to ignore late events of aborted batches
	bool scheduling_downloads = false;            ///< Whether we're currently inside StartNextDownloads() (guards against re-entrancy)

	std::optional<FileHandle> cur_file;           ///< Currently downloaded file (TCP fallback path; downloads one file at a time)
	std::unique_ptr<ContentInfo> cur_info;        ///< Information about the currently downloaded file (TCP fallback path)
	bool is_connecting = false;                   ///< Whether we're connecting
	bool is_cancelled = false;                    ///< Whether the download has been cancelled
	std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now(); ///< The last time there was network activity

	friend class NetworkContentConnecter;
	friend class ContentDownload;
	friend class ContentManifestDownload;

	bool ReceiveServerInfo(Packet &p) override;
	bool ReceiveServerContent(Packet &p) override;

	ContentInfo *GetContent(ContentID cid);
	const ContentInfo *GetContent(ContentID cid) const { return const_cast<ClientNetworkContentSocketHandler *>(this)->GetContent(cid); }
	void DownloadContentInfo(ContentID cid);

	void OnConnect(bool success) override;
	void OnDisconnect() override;
	void OnReceiveContentInfo(const ContentInfo &ci) override;
	void OnDownloadProgress(const ContentInfo &ci, int bytes) override;
	void OnDownloadComplete(ContentID cid) override;

	void OnManifestFailed(uint32_t batch);
	void OnManifestComplete(uint32_t batch, std::string_view response);
	bool IsCancelled() const;

	bool BeforeDownload();
	void AfterDownload();

	bool ParseContentManifest(std::string_view buffer);
	void StartNextDownloads();
	void OnDownloadFailed(const ContentInfo &ci, uint32_t batch);
	void AbortActiveDownloads();

	void DownloadSelectedContentHTTP(const ContentIDList &content);
	void DownloadSelectedContentFallback(const ContentIDList &content);
public:
	/** The idle timeout; when to close the connection because it's idle. */
	static constexpr std::chrono::seconds IDLE_TIMEOUT = std::chrono::seconds(60);

	void Connect();
	void SendReceive();
	NetworkRecvStatus CloseConnection(bool error = true) override;
	void Cancel();

	void RequestContentList(ContentType type);
	void RequestContentList(std::span<const ContentID> content_ids);
	void RequestContentList(ContentVector *cv, bool send_md5sum = true);

	void DownloadSelectedContent(uint &files, uint &bytes, bool fallback = false);
	void RequestQueuedContentInfo();

	void Select(ContentID cid);
	void Unselect(ContentID cid);
	void SelectAll();
	void SelectUpgrade();
	void UnselectAll();
	void ToggleSelectedState(const ContentInfo &ci);

	void ReverseLookupDependency(ConstContentVector &parents, const ContentInfo &child) const;
	void ReverseLookupTreeDependency(ConstContentVector &tree, const ContentInfo *child) const;
	void CheckDependencyState(const ContentInfo &ci);

	/**
	 * Get a read-only view of content info for iterating externally.
	 * @return Read-only view of content info.
	 */
	auto Info() const { return this->infos | std::views::transform([](const auto &ci) -> const ContentInfo & { return *ci; }); }

	void Clear();

	/**
	 * Add a callback to this class, if it doesn't already exist.
	 * @param cb The callback to add.
	 */
	void AddCallback(ContentCallback *cb) { include(this->callbacks, cb); }

	/**
	 * Remove a callback.
	 * @param cb The callback to remove.
	 */
	void RemoveCallback(ContentCallback *cb) { this->callbacks.erase(std::ranges::find(this->callbacks, cb)); }
};

extern ClientNetworkContentSocketHandler _network_content_client;

void ShowNetworkContentListWindow(ContentVector *cv = nullptr, ContentType type1 = ContentType::End, ContentType type2 = ContentType::End);

void ShowMissingContentWindow(const GRFConfigList &list);

#endif /* NETWORK_CONTENT_H */
