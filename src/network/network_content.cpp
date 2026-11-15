/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file network_content.cpp Content sending/receiving part of the network protocol. */

#include "../stdafx.h"
#include "../rev.h"
#include <atomic>
#include "../ai/ai.hpp"
#include "../game/game.hpp"
#include "../window_func.h"
#include "../error.h"
#include "../fileio_func.h"
#include "../newgrf.h"
#include "../base_media_base.h"
#include "../base_media_graphics.h"
#include "../base_media_music.h"
#include "../base_media_sounds.h"
#include "../settings_type.h"
#include "../strings_func.h"
#include "../time_chrono.h"
#include "../timer/timer.h"
#include "../timer/timer_window.h"
#include "../core/string_consumer.hpp"
#include "network_content.h"

#include "table/strings.h"

#if defined(WITH_ZLIB)
#include <zlib.h>
#endif

#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#endif

#include "../safeguards.h"

extern bool HasScenario(const ContentInfo &ci, bool md5sum);

/** The client we use to connect to the server. */
ClientNetworkContentSocketHandler _network_content_client;

/** Check whether NewGRF content exists. @copydoc HasContentProc */
static bool HasGRFConfig(const ContentInfo &ci, bool md5sum)
{
	return FindGRFConfig(UnflattenNewGRFLabel<GrfID>(std::byteswap(ci.unique_id)), md5sum ? FindGRFConfigMode::Exact : FindGRFConfigMode::Any, md5sum ? &ci.md5sum : nullptr) != nullptr;
}

/**
 * Check whether a function piece of content is locally known.
 * Matches on the unique ID and possibly the MD5 checksum.
 * @param ci     the content info to search for
 * @param md5sum also match the MD5 checksum?
 * @return true iff it's known
 */
using HasContentProc = bool(const ContentInfo &ci, bool md5sum);

/**
 * Get the has-content check function for the given content type.
 * @param type Content type to get check function for.
 * @return Check function pointer.
 */
static HasContentProc *GetHasContentProcforContentType(ContentType type)
{
	switch (type) {
		case ContentType::NewGRF: return HasGRFConfig;
		case ContentType::BaseGraphics: return BaseGraphics::HasSet;
		case ContentType::BaseMusic: return BaseMusic::HasSet;
		case ContentType::BaseSounds: return BaseSounds::HasSet;
		case ContentType::Ai: return AI::HasAI;
		case ContentType::AiLibrary: return AI::HasAILibrary;
		case ContentType::Gs: return Game::HasGame;
		case ContentType::GsLibrary: return Game::HasGameLibrary;
		case ContentType::Scenario: return HasScenario;
		case ContentType::Heightmap: return HasScenario;
		default: return nullptr;
	}
}

bool ClientNetworkContentSocketHandler::ReceiveServerInfo(Packet &p)
{
	auto ci = std::make_unique<ContentInfo>();
	ci->type     = (ContentType)p.Recv_uint8();
	ci->id       = (ContentID)p.Recv_uint32();
	ci->filesize = p.Recv_uint32();

	ci->name        = p.Recv_string(NETWORK_CONTENT_NAME_LENGTH);
	ci->version     = p.Recv_string(NETWORK_CONTENT_VERSION_LENGTH);
	ci->url         = p.Recv_string(NETWORK_CONTENT_URL_LENGTH);
	ci->description = p.Recv_string(NETWORK_CONTENT_DESC_LENGTH, {StringValidationSetting::ReplaceWithQuestionMark, StringValidationSetting::AllowNewline});

	ci->unique_id = p.Recv_uint32();
	p.Recv_bytes(ci->md5sum);

	uint dependency_count = p.Recv_uint8();
	ci->dependencies.reserve(dependency_count);
	for (uint i = 0; i < dependency_count; i++) {
		ContentID dependency_cid = (ContentID)p.Recv_uint32();
		ci->dependencies.push_back(dependency_cid);
		this->reverse_dependency_map.insert({ dependency_cid, ci->id });
	}

	uint tag_count = p.Recv_uint8();
	ci->tags.reserve(tag_count);
	for (uint i = 0; i < tag_count; i++) ci->tags.push_back(p.Recv_string(NETWORK_CONTENT_TAG_LENGTH));

	if (!ci->IsValid()) {
		this->CloseConnection();
		return false;
	}

	HasContentProc *proc = GetHasContentProcforContentType(ci->type);
	if (proc != nullptr) {
		if (proc(*ci, true)) {
			ci->state = ContentInfo::State::AlreadyHere;
		} else {
			ci->state = ContentInfo::State::Unselected;
			if (proc(*ci, false)) ci->upgrade = true;
		}
	} else {
		ci->state = ContentInfo::State::Unselected;
	}

	/* Something we don't have and has filesize 0 does not exist in the system */
	if (ci->state == ContentInfo::State::Unselected && ci->filesize == 0) ci->state = ContentInfo::State::DoesNotExist;

	/* Do we already have a stub for this? */
	for (const auto &ici : this->infos) {
		if (ici->type == ci->type && ici->unique_id == ci->unique_id && ci->md5sum == ici->md5sum) {
			/* Preserve the name if possible */
			if (ci->name.empty()) ci->name = ici->name;
			if (ici->IsSelected()) ci->state = ici->state;

			/*
			 * As ici might be selected by the content window we cannot delete that.
			 * However, we want to keep most of the values of ci, except the values
			 * we (just) already preserved.
			 */
			*ici = *ci;

			this->OnReceiveContentInfo(*ici);
			return true;
		}
	}

	/* Missing content info? Don't list it */
	if (ci->filesize == 0) {
		return true;
	}

	ContentInfo *info = this->infos.emplace_back(std::move(ci)).get();

	/* Incoming data means that we might need to reconsider dependencies */
	ConstContentVector parents;
	this->ReverseLookupTreeDependency(parents, info);
	for (const ContentInfo *ici : parents) {
		this->CheckDependencyState(*ici);
	}

	this->OnReceiveContentInfo(*info);

	return true;
}

/**
 * Request the content list for the given type.
 * @param type The content type to request the list for.
 */
void ClientNetworkContentSocketHandler::RequestContentList(ContentType type)
{
	if (type == ContentType::End) {
		this->RequestContentList(ContentType::BaseGraphics);
		this->RequestContentList(ContentType::BaseMusic);
		this->RequestContentList(ContentType::BaseSounds);
		this->RequestContentList(ContentType::Scenario);
		this->RequestContentList(ContentType::Heightmap);
		this->RequestContentList(ContentType::Ai);
		this->RequestContentList(ContentType::AiLibrary);
		this->RequestContentList(ContentType::Gs);
		this->RequestContentList(ContentType::GsLibrary);
		this->RequestContentList(ContentType::NewGRF);
		return;
	}

	this->Connect();

	auto p = std::make_unique<Packet>(this, PacketContentType::ClientInfoList);
	p->Send_uint8 (to_underlying(type));
	p->Send_uint32(0xffffffff);
	p->Send_uint8 (2);
	p->Send_string("vanilla");
	p->Send_string(_openttd_content_version);

	/* Patchpacks can extend the list with one. In BaNaNaS metadata you can
	 * add a branch in the 'compatibility' list, to filter on this. If you want
	 * your patchpack to be mentioned in the BaNaNaS web-interface, create an
	 * issue on https://github.com/OpenTTD/bananas-api asking for this.
	 */

	p->Send_string("jgrpp");
	p->Send_string(_openttd_release_version);

	this->SendPacket(std::move(p));
}

/**
 * Request the content list for a given number of content IDs.
 * @param content_ids The unique identifiers of the content to request information about.
 */
void ClientNetworkContentSocketHandler::RequestContentList(std::span<const ContentID> content_ids)
{
	/* We can "only" send a limited number of IDs in a single packet.
	 * A packet begins with the packet size and a byte for the type.
	 * Then this packet adds a uint16_t for the count in this packet.
	 * The rest of the packet can be used for the IDs. */
	static constexpr size_t MAX_CONTENT_IDS_PER_PACKET = (TCP_MTU - sizeof(PacketSize) - sizeof(uint8_t) - sizeof(uint16_t)) / sizeof(uint32_t);

	if (content_ids.empty()) return;

	this->Connect();

	for (auto it = std::begin(content_ids); it != std::end(content_ids); /* nothing */) {
		auto last = std::ranges::next(it, MAX_CONTENT_IDS_PER_PACKET, std::end(content_ids));

		auto p = std::make_unique<Packet>(this, PacketContentType::ClientInfoID, TCP_MTU);
		p->Send_uint16(std::distance(it, last));

		for (; it != last; ++it) {
			p->Send_uint32(*it);
		}

		this->SendPacket(std::move(p));
	}
}

/**
 * Request the content list for a list of content.
 * @param cv List with unique IDs and MD5 checksums.
 * @param send_md5sum Whether we want a MD5 checksum matched set of files or not.
 */
void ClientNetworkContentSocketHandler::RequestContentList(ContentVector *cv, bool send_md5sum)
{
	if (cv == nullptr) return;

	this->Connect();

	const uint max_per_packet = std::min<uint>(255, (TCP_MTU - sizeof(PacketSize) - sizeof(uint8_t) - sizeof(uint8_t)) /
			(sizeof(uint8_t) + sizeof(uint32_t) + (send_md5sum ? MD5_HASH_BYTES : 0))) - 1;

	uint offset = 0;

	while (cv->size() > offset) {
		auto p = std::make_unique<Packet>(this, send_md5sum ? PacketContentType::ClientInfoExternalIDMD5 : PacketContentType::ClientInfoExternalID, TCP_MTU);
		const uint to_send = std::min<uint>(static_cast<uint>(cv->size() - offset), max_per_packet);
		p->Send_uint8(static_cast<uint8_t>(to_send));

		for (uint i = 0; i < to_send; i++) {
			const ContentInfo *ci = (*cv)[offset + i].get();
			p->Send_uint8((uint8_t)ci->type);
			p->Send_uint32(ci->unique_id);
			if (!send_md5sum) continue;
			p->Send_bytes(ci->md5sum);
		}

		this->SendPacket(std::move(p));

		offset += to_send;
	}

	for (std::unique_ptr<ContentInfo> &ci : *cv) {
		bool found = false;
		for (const std::unique_ptr<ContentInfo> &ci2 : this->infos) {
			if (ci->type == ci2->type && ci->unique_id == ci2->unique_id &&
					(!send_md5sum || ci->md5sum == ci2->md5sum)) {
				found = true;
				break;
			}
		}
		if (!found) {
			this->infos.push_back(std::move(ci));
		}
	}
}

/**
 * Actually begin downloading the content we selected.
 * @param[out] files The number of files we are going to download.
 * @param[out] bytes The number of bytes we are going to download.
 * @param fallback Whether to use the fallback or not.
 */
void ClientNetworkContentSocketHandler::DownloadSelectedContent(uint &files, uint &bytes, bool fallback)
{
	bytes = 0;

	ContentIDList content;
	for (const std::unique_ptr<ContentInfo> &ci : this->infos) {
		if (!ci->IsSelected() || ci->state == ContentInfo::State::AlreadyHere) continue;

		content.push_back(ci->id);
		bytes += ci->filesize;
	}

	files = (uint)content.size();

	/* If there's nothing to download, do nothing. */
	if (files == 0) return;

	this->is_cancelled = false;

	if (fallback) {
		this->DownloadSelectedContentFallback(content);
	} else {
		/* Abort any downloads of a previous batch that are still running. */
		this->pending_downloads.clear();
		this->AbortActiveDownloads();

		this->download_batch++;
		this->DownloadSelectedContentHTTP(content);
	}
}

/**
 * Initiate downloading the content over HTTP.
 * @param content The content to download.
 */
void ClientNetworkContentSocketHandler::DownloadSelectedContentHTTP(const ContentIDList &content)
{
	format_buffer content_request;
	for (const ContentID &id : content) {
		content_request.format("{}\n", id);
	}

	this->http_response_index = -1;

	/* The handler is a singleton that outlives the request; share it without taking ownership. */
	NetworkHTTPSocketHandler::Connect(NetworkContentMirrorUriString(), std::shared_ptr<HTTPCallback>(static_cast<HTTPCallback *>(this), [](HTTPCallback *) {}), content_request.to_string());
}

/**
 * Initiate downloading the content over the fallback protocol.
 * @param content The content to download.
 */
void ClientNetworkContentSocketHandler::DownloadSelectedContentFallback(const ContentIDList &content)
{
	uint count = (uint)content.size();
	const ContentID *content_ids = content.data();
	this->Connect();

	while (count > 0) {
		/* We can "only" send a limited number of IDs in a single packet.
		 * A packet begins with the packet size and a byte for the type.
		 * Then this packet adds a uint16_t for the count in this packet.
		 * The rest of the packet can be used for the IDs. */
		uint p_count = std::min<uint>(count, (TCP_MTU - sizeof(PacketSize) - sizeof(uint8_t) - sizeof(uint16_t)) / sizeof(uint32_t));

		auto p = std::make_unique<Packet>(this, PacketContentType::ClientContent, TCP_MTU);
		p->Send_uint16(p_count);

		for (uint i = 0; i < p_count; i++) {
			p->Send_uint32(content_ids[i]);
		}

		this->SendPacket(std::move(p));
		count -= p_count;
		content_ids += p_count;
	}
}

/**
 * Determine the full filename of a piece of content information
 * @param ci         the information to get the filename from
 * @param compressed should the filename end with .gz?
 * @return a statically allocated buffer with the filename or
 *         nullptr when no filename could be made.
 */
static std::string GetFullFilename(const ContentInfo &ci, bool compressed)
{
	Subdirectory dir = GetContentInfoSubDir(ci.type);
	if (dir == Subdirectory::None) return {};

	std::string buf = FioGetDirectory(Searchpath::AutodownloadDir, dir);
	buf += ci.filename;
	buf += compressed ? ".tar.gz" : ".tar";

	return buf;
}

/**
 * Gunzip a given file and remove the .gz if successful.
 * @param ci container with filename
 * @return true if the gunzip completed
 */
static bool GunzipFile(const ContentInfo &ci)
{
#if defined(WITH_ZLIB)
	bool ret = true;

	/* Need to open the file with fopen() to support non-ASCII on Windows. */
	auto ftmp = FileHandle::Open(GetFullFilename(ci, true), "rb");
	if (!ftmp.has_value()) return false;
	/* Duplicate the handle, and close the FILE*, to avoid double-closing the handle later. */
	int fdup = dup(fileno(*ftmp));
	gzFile fin = gzdopen(fdup, "rb");
	ftmp.reset();

	auto fout = FileHandle::Open(GetFullFilename(ci, false), "wb");

	if (fin == nullptr || !fout.has_value()) {
		ret = false;
	} else {
		uint8_t buff[8192];
		for (;;) {
			int read = gzread(fin, buff, sizeof(buff));
			if (read == 0) {
				/* If gzread() returns 0, either the end-of-file has been
				 * reached or an underlying read error has occurred.
				 *
				 * gzeof() can't be used, because:
				 * 1.2.5 - it is safe, 1 means 'everything was OK'
				 * 1.2.3.5, 1.2.4 - 0 or 1 is returned 'randomly'
				 * 1.2.3.3 - 1 is returned for truncated archive
				 *
				 * So we use gzerror(). When proper end of archive
				 * has been reached, then:
				 * errnum == Z_STREAM_END in 1.2.3.3,
				 * errnum == 0 in 1.2.4 and 1.2.5 */
				int errnum;
				gzerror(fin, &errnum);
				if (errnum != 0 && errnum != Z_STREAM_END) ret = false;
				break;
			}
			if (read < 0 || static_cast<size_t>(read) != fwrite(buff, 1, read, *fout)) {
				/* If gzread() returns -1, there was an error in archive */
				ret = false;
				break;
			}
			/* DO NOT DO THIS! It will fail to detect broken archive with 1.2.3.3!
			 * if (read < sizeof(buff)) break; */
		}
	}

	if (fin != nullptr) {
		gzclose(fin);
	} else if (fdup != -1) {
		/* Failing gzdopen does not close the passed file descriptor. */
		close(fdup);
	}

	return ret;
#else
	NOT_REACHED();
#endif /* defined(WITH_ZLIB) */
}

/**
 * Handle the extracting of a file after downloading it has been done.
 * @param ci container with filename
 * @return true iff the file was extracted successfully.
 */
static bool FinishDownload(const ContentInfo &ci)
{
	if (GunzipFile(ci)) {
		FioRemove(GetFullFilename(ci, true));

		Subdirectory sd = GetContentInfoSubDir(ci.type);
		if (sd == Subdirectory::None) NOT_REACHED();

		TarScanner ts;
		std::string fname = GetFullFilename(ci, false);
		ts.AddFile(sd, fname);

		if (ci.type == ContentType::BaseMusic) {
			/* Music can't be in a tar. So extract the tar! */
			ExtractTar(fname, Subdirectory::Baseset);
			FioRemove(fname);
		}

#ifdef __EMSCRIPTEN__
		EM_ASM(if (window["openttd_syncfs"]) openttd_syncfs());
#endif

		return true;
	}

	ShowErrorMessage(GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_EXTRACT), {}, WarningLevel::Error);
	return false;
}

/**
 * A single content file being downloaded over HTTP.
 *
 * Every download has its own file handle and HTTP request, so multiple files
 * can be downloaded at the same time. The HTTP layer shares ownership of this
 * object, so it stays alive until the HTTP request itself is gone.
 */
class ContentDownload : public HTTPCallback, public std::enable_shared_from_this<ContentDownload> {
public:
	ContentDownload(ClientNetworkContentSocketHandler &parent, std::unique_ptr<ContentInfo> info, std::string url, uint32_t batch) :
		parent(parent),
		info(std::move(info)),
		url(std::move(url)),
		batch(batch)
	{
	}

	const ContentInfo &GetInfo() const { return *this->info; }
	const std::string &GetUrl() const { return this->url; }

	/**
	 * Open the file to write the downloaded data to.
	 * @return false on any error.
	 */
	bool OpenFile()
	{
		if (this->info->filesize != 0) {
			/* The filesize is > 0, so we are going to download it */
			std::string filename = GetFullFilename(*this->info, true);
			if (filename.empty() || !(this->file = FileHandle::Open(filename, "wb")).has_value()) {
				/* Unless that fails of course... */
				return false;
			}
		}
		return true;
	}

	/** Start downloading the file. */
	void Start()
	{
		NetworkHTTPSocketHandler::Connect(this->url, this->shared_from_this());
	}

	/**
	 * Abort the download, e.g. because a new batch of downloads was started.
	 * The HTTP request will be cancelled as soon as possible; any data it still
	 * delivers is ignored.
	 */
	void Abort()
	{
		this->aborted = true;
		this->finished = true;
		this->file.reset();
	}

	bool IsFinished() const { return this->finished; }

	void OnFailure() override
	{
		if (this->finished) return;
		this->finished = true;

		this->file.reset();
		this->parent.OnDownloadFailed(*this->info, this->batch);
		this->parent.StartNextDownloads();
	}

	void OnReceiveData(UniqueBuffer<char> data) override
	{
		assert(data.get() == nullptr || data.size() != 0);

		/* Ignore any latent data coming from a finished or aborted download. */
		if (this->finished) return;

		if (data != nullptr) {
			/* We have data, so write it to the file. */
			if (this->file.has_value() && fwrite(data.get(), 1, data.size(), *this->file) == data.size()) {
				/* Just received the data. */
				this->parent.OnDownloadProgress(*this->info, (int)data.size());
			} else {
				/* Writing failed somehow; mark this download as failed. */
				this->OnFailure();
			}

			/* Nothing more to do now. */
			return;
		}

		/* We've finished downloading the file. */
		this->finished = true;
		this->file.reset();

		if (FinishDownload(*this->info)) {
			this->parent.OnDownloadComplete(this->info->id);
		} else {
			this->parent.OnDownloadFailed(*this->info, this->batch);
		}
		this->parent.StartNextDownloads();
	}

	bool IsCancelled() const override
	{
		return this->aborted || this->parent.IsCancelled();
	}

private:
	ClientNetworkContentSocketHandler &parent; ///< The handler this download belongs to.
	std::unique_ptr<ContentInfo> info;         ///< Information about the file being downloaded.
	const std::string url;                     ///< URL to download the file from.
	std::optional<FileHandle> file;            ///< The file we are writing the download to.
	uint32_t batch;                            ///< The download batch we belong to.
	std::atomic<bool> aborted = false;         ///< Whether the download was aborted.
	bool finished = false;                     ///< Whether the download has finished or failed.
};

/** Abort all downloads that are currently running. */
void ClientNetworkContentSocketHandler::AbortActiveDownloads()
{
	for (auto &download : this->active_downloads) download->Abort();
}

bool ClientNetworkContentSocketHandler::ReceiveServerContent(Packet &p)
{
	if (!this->cur_file.has_value()) {
		/* When we haven't opened a file this must be our first packet with metadata. */
		this->cur_info = std::make_unique<ContentInfo>();
		this->cur_info->type     = (ContentType)p.Recv_uint8();
		this->cur_info->id       = (ContentID)p.Recv_uint32();
		this->cur_info->filesize = p.Recv_uint32();
		this->cur_info->filename = p.Recv_string(NETWORK_CONTENT_FILENAME_LENGTH);

		if (!this->BeforeDownload()) {
			this->CloseConnection();
			return false;
		}
	} else {
		/* We have a file opened, thus are downloading internal content */
		ssize_t to_read = p.RemainingBytesToTransfer();
		auto write_to_disk = [this](std::span<const uint8_t> buffer) {
			return fwrite(buffer.data(), 1, buffer.size(), *this->cur_file);
		};
		if (to_read != 0 && p.TransferOut(write_to_disk) != to_read) {
			CloseWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::ContentDownload);
			ShowErrorMessage(
				GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_DOWNLOAD),
				GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_DOWNLOAD_FILE_NOT_WRITABLE),
				WarningLevel::Error);
			this->CloseConnection();
			this->cur_file.reset();

			return false;
		}

		this->OnDownloadProgress(*this->cur_info, (int)to_read);

		if (to_read == 0) this->AfterDownload();
	}

	return true;
}

/**
 * Handle the opening of the file before downloading.
 * @return false on any error.
 */
bool ClientNetworkContentSocketHandler::BeforeDownload()
{
	if (!this->cur_info->IsValid()) {
		this->cur_info.reset();
		return false;
	}

	if (this->cur_info->filesize != 0) {
		/* The filesize is > 0, so we are going to download it */
		std::string filename = GetFullFilename(*this->cur_info, true);
		if (filename.empty() || !(this->cur_file = FileHandle::Open(filename, "wb")).has_value()) {
			/* Unless that fails of course... */
			CloseWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::ContentDownload);
			ShowErrorMessage(
				GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_DOWNLOAD),
				GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_DOWNLOAD_FILE_NOT_WRITABLE),
				WarningLevel::Error);
			return false;
		}
	}
	return true;
}

/**
 * Handle the closing and extracting of a file after
 * downloading it has been done.
 */
void ClientNetworkContentSocketHandler::AfterDownload()
{
	/* We read nothing; that's our marker for end-of-stream.
	 * Now gunzip the tar and make it known. */
	this->cur_file.reset();

	if (FinishDownload(*this->cur_info)) {
		this->OnDownloadComplete(this->cur_info->id);
	}
}

bool ClientNetworkContentSocketHandler::IsCancelled() const
{
	return this->is_cancelled;
}

/* Also called to just clean up the mess. */
void ClientNetworkContentSocketHandler::OnFailure()
{
	this->http_response.clear();
	this->http_response.shrink_to_fit();
	this->http_response_index = -2;

	/* If we fail, download the rest via the 'old' system. */
	if (!this->is_cancelled) {
		uint files, bytes;

		this->DownloadSelectedContent(files, bytes, true);
	}
}

void ClientNetworkContentSocketHandler::OnReceiveData(UniqueBuffer<char> data)
{
	assert(data.get() == nullptr || data.size() != 0);

	/* Ignore any latent data coming from a connection we closed. */
	if (this->http_response_index == -2) {
		return;
	}

	if (this->http_response_index == -1) {
		if (data != nullptr) {
			/* Append the rest of the response. */
			this->http_response.insert(this->http_response.end(), data.get(), data.get() + data.size());
			return;
		} else {
			/* Make sure the response is properly terminated. */
			this->http_response.push_back('\0');

			/* And prepare for parsing the response. */
			this->http_response_index = 0;
		}
	}

	/* The manifest is fully buffered; any further data of this connection is unexpected,
	 * as the file downloads each have their own callback. */
	if (data != nullptr) return;

	if (!this->ParseContentManifest()) {
		this->OnFailure();
		return;
	}

	/* The manifest has been consumed; ignore any latent data of this connection. */
	this->http_response.clear();
	this->http_response.shrink_to_fit();
	this->http_response_index = -2;

	this->StartNextDownloads();
}

/**
 * Parse the manifest received from the content mirror into the queue of pending downloads.
 * @return true iff the manifest could be parsed.
 */
bool ClientNetworkContentSocketHandler::ParseContentManifest()
{
	this->pending_downloads.clear();
	this->failed_downloads = 0;

	std::string_view buffer{this->http_response.data(), this->http_response.size()};
	/* An empty manifest means the mirror knows nothing about the requested content; retry via the fallback system. */
	if (buffer.size() <= 1) return false;

	/* The last byte is the '\0' terminator we added, and not part of a line. */
	size_t end = buffer.size() - 1;
	size_t offset = 0;

	try {
		while (offset < end) {
			std::string_view line = buffer.substr(offset);
			auto len = line.find('\n');
			if (len == std::string_view::npos) throw std::exception{};
			line = line.substr(0, len);
			offset += len + 1;

			auto ci = std::make_unique<ContentInfo>();
			StringConsumer consumer{line};

			/* Read the ID */
			ci->id = static_cast<ContentID>(consumer.ReadIntegerBase<uint>(10));
			if (!consumer.ReadIf(",")) throw std::exception{};

			/* Read the type */
			ci->type = static_cast<ContentType>(consumer.ReadIntegerBase<uint>(10));
			if (!consumer.ReadIf(",")) throw std::exception{};

			/* Read the file size */
			ci->filesize = consumer.ReadIntegerBase<uint32_t>(10);
			if (!consumer.ReadIf(",")) throw std::exception{};

			/* Read the URL */
			auto url = consumer.GetLeftData();

			/* Is it a fallback URL? If so, download it via the fallback system instead. */
			if (consumer.ReadIf("ottd")) {
				this->failed_downloads++;
				continue;
			}

			consumer.SkipUntilChar('/', StringConsumer::KEEP_SEPARATOR);
			std::string_view filename;
			/* Skip all but the last part. There must be at least one / though */
			do {
				if (!consumer.ReadIf("/")) throw std::exception{};
				filename = consumer.ReadUntilChar('/', StringConsumer::KEEP_SEPARATOR);
			} while (consumer.AnyBytesLeft());

			/* Remove the extension from the string. */
			for (uint i = 0; i < 2; i++) {
				auto pos = filename.find_last_of('.');
				if (pos == std::string::npos) throw std::exception{};
				filename = filename.substr(0, pos);
			}

			/* Copy the string, without extension, to the filename. */
			ci->filename = filename;

			this->pending_downloads.emplace_back(std::move(ci), std::string(url));
		}
	} catch (const std::exception&) {
		return false;
	}

	return true;
}

/**
 * Start as many pending downloads as the configured concurrency allows.
 *
 * Called whenever a download finishes or fails, to refill the set of
 * concurrent downloads, and to finalise the batch when everything is done.
 */
void ClientNetworkContentSocketHandler::StartNextDownloads()
{
	/* A re-entrant call means the outer invocation is still busy starting downloads; it will handle the work. */
	if (this->scheduling_downloads) {
		return;
	}
	this->scheduling_downloads = true;

	/* Prune finished downloads. The HTTP layer keeps them alive for as long as their request is running. */
	std::erase_if(this->active_downloads, [](const auto &download) { return download->IsFinished(); });

	while (this->active_downloads.size() < _settings_client.network.max_concurrent_content_downloads && !this->pending_downloads.empty()) {
		auto entry = std::move(this->pending_downloads.front());
		this->pending_downloads.erase(this->pending_downloads.begin());

		std::unique_ptr<ContentInfo> info = std::move(entry.first);
		if (!info->IsValid()) {
			this->OnDownloadFailed(*info, this->download_batch);
			continue;
		}

		auto download = std::make_shared<ContentDownload>(*this, std::move(info), std::move(entry.second), this->download_batch);

		if (!download->OpenFile()) {
			ShowErrorMessage(
				GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_DOWNLOAD),
				GetEncodedString(STR_CONTENT_ERROR_COULD_NOT_DOWNLOAD_FILE_NOT_WRITABLE),
				WarningLevel::Error);
			this->OnDownloadFailed(download->GetInfo(), this->download_batch);
			continue;
		}

		download->Start();

		if (download->IsFinished()) {
			/* The request failed synchronously, e.g. when no HTTP backend was compiled in. */
			continue;
		}

		this->active_downloads.push_back(std::move(download));
	}

	this->scheduling_downloads = false;

	/* The whole batch is done; retry any failed downloads via the fallback system. */
	if (this->pending_downloads.empty() && this->active_downloads.empty() && this->failed_downloads > 0 && !this->is_cancelled) {
		this->failed_downloads = 0;
		uint files, bytes;
		this->DownloadSelectedContent(files, bytes, true);
	}
}

/**
 * Handle the failure of a single file download.
 * @param ci    The content info of the file that failed to download.
 * @param batch The download batch the failure belongs to.
 */
void ClientNetworkContentSocketHandler::OnDownloadFailed(const ContentInfo &ci, uint32_t batch)
{
	if (batch != this->download_batch) {
		return;
	}

	this->failed_downloads++;

	/* Tell the window this file has been reset. */
	this->OnDownloadProgress(ci, -1);
}

/** Connect to the content server. */
class NetworkContentConnecter : public TCPConnecter {
public:
	/**
	 * Initiate the connecting.
	 * @param connection_string The address of the server.
	 */
	NetworkContentConnecter(std::string_view connection_string) : TCPConnecter(connection_string, NETWORK_CONTENT_SERVER_PORT) {}

	void OnFailure() override
	{
		_network_content_client.is_connecting = false;
		_network_content_client.OnConnect(false);
	}

	void OnConnect(SOCKET s) override
	{
		assert(_network_content_client.sock == INVALID_SOCKET);
		_network_content_client.last_activity = std::chrono::steady_clock::now();
		_network_content_client.is_connecting = false;
		_network_content_client.sock = s;
		_network_content_client.Reopen();
		_network_content_client.OnConnect(true);
	}
};

/**
 * Connect with the content server.
 */
void ClientNetworkContentSocketHandler::Connect()
{
	if (this->sock != INVALID_SOCKET || this->is_connecting) return;

	this->is_cancelled = false;
	this->is_connecting = true;

	TCPConnecter::Create<NetworkContentConnecter>(NetworkContentServerConnectionString());
}

/** Disconnect from the content server. @copydoc NetworkTCPSocketHandler::CloseConnection */
NetworkRecvStatus ClientNetworkContentSocketHandler::CloseConnection([[maybe_unused]] bool error)
{
	NetworkContentSocketHandler::CloseConnection();

	if (this->sock == INVALID_SOCKET) return NetworkRecvStatus::Okay;

	this->CloseSocket();
	this->OnDisconnect();

	return NetworkRecvStatus::Okay;
}

/**
 * Cancel the current download.
 */
void ClientNetworkContentSocketHandler::Cancel(void)
{
	this->is_cancelled = true;
	this->pending_downloads.clear();
	this->CloseConnection();
}

/**
 * Check whether we received/can send some data from/to the content server and
 * when that's the case handle it appropriately
 */
void ClientNetworkContentSocketHandler::SendReceive()
{
	if (this->sock == INVALID_SOCKET || this->is_connecting) return;

	/* Close the connection to the content server after inactivity; there can still be downloads pending via HTTP. */
	if (std::chrono::steady_clock::now() > this->last_activity + IDLE_TIMEOUT) {
		this->CloseConnection();
		return;
	}

	if (this->CanSendReceive()) {
		if (this->ReceivePackets()) {
			/* Only update activity once a packet is received, instead of every time we try it. */
			this->last_activity = std::chrono::steady_clock::now();
		}
	}

	this->SendPackets();
}

/** Timeout after queueing content for it to try to be requested. */
static constexpr auto CONTENT_QUEUE_TIMEOUT = std::chrono::milliseconds(100);

/** Timer delay requesting content, so it can be batched more efficiently in asynchronous contexts. */
static TimeoutTimer<TimerWindow> _request_queue_timeout = {CONTENT_QUEUE_TIMEOUT, []() {
	_network_content_client.RequestQueuedContentInfo();
}};

/**
 * Download information of a given Content ID if not already tried
 * @param cid the ID to try
 */
void ClientNetworkContentSocketHandler::DownloadContentInfo(ContentID cid)
{
	/* When we tried to download it already, don't try again */
	if (std::ranges::find(this->requested, cid) != this->requested.end()) return;

	this->requested.push_back(cid);
	this->queued.push_back(cid);
	_request_queue_timeout.Reset();
}

/**
 * Send a content request for queued content info download.
 */
void ClientNetworkContentSocketHandler::RequestQueuedContentInfo()
{
	if (this->queued.empty()) return;

	/* Wait until we've briefly stopped receiving data (which will contain more content) before making the request. */
	if (std::chrono::steady_clock::now() <= this->last_activity + CONTENT_QUEUE_TIMEOUT) {
		_request_queue_timeout.Reset();
		return;
	}

	/* Move the queue locally so more ids can be queued for later. */
	ContentIDList queue;
	queue.swap(this->queued);

	/* Remove ids that have since been received since the request was queued. */
	queue.erase(std::remove_if(std::begin(queue), std::end(queue), [this](ContentID content_id) {
		return std::ranges::find(this->infos, content_id, &ContentInfo::id) != std::end(this->infos);
	}), std::end(queue));

	this->RequestContentList(queue);
}

/**
 * Get the content info based on a ContentID
 * @param cid the ContentID to search for
 * @return the ContentInfo or nullptr if not found
 */
ContentInfo *ClientNetworkContentSocketHandler::GetContent(ContentID cid)
{
	for (const std::unique_ptr<ContentInfo> &ci : this->infos) {
		if (ci->id == cid) return ci.get();
	}
	return nullptr;
}

/**
 * Select a specific content id.
 * @param cid the content ID to select
 */
void ClientNetworkContentSocketHandler::Select(ContentID cid)
{
	ContentInfo *ci = this->GetContent(cid);
	if (ci == nullptr || ci->state != ContentInfo::State::Unselected) return;

	ci->state = ContentInfo::State::Selected;
	this->CheckDependencyState(*ci);
}

/**
 * Unselect a specific content id.
 * @param cid the content ID to deselect
 */
void ClientNetworkContentSocketHandler::Unselect(ContentID cid)
{
	ContentInfo *ci = this->GetContent(cid);
	if (ci == nullptr || !ci->IsSelected()) return;

	ci->state = ContentInfo::State::Unselected;
	this->CheckDependencyState(*ci);
}

/** Select everything we can select */
void ClientNetworkContentSocketHandler::SelectAll()
{
	for (const std::unique_ptr<ContentInfo> &ci : this->infos) {
		if (ci->state == ContentInfo::State::Unselected) {
			ci->state = ContentInfo::State::Selected;
			this->CheckDependencyState(*ci);
		}
	}
}

/** Select everything that's an update for something we've got */
void ClientNetworkContentSocketHandler::SelectUpgrade()
{
	for (const std::unique_ptr<ContentInfo> &ci : this->infos) {
		if (ci->state == ContentInfo::State::Unselected && ci->upgrade) {
			ci->state = ContentInfo::State::Selected;
			this->CheckDependencyState(*ci);
		}
	}
}

/** Unselect everything that we've not downloaded so far. */
void ClientNetworkContentSocketHandler::UnselectAll()
{
	for (const std::unique_ptr<ContentInfo> &ci : this->infos) {
		if (ci->IsSelected() && ci->state != ContentInfo::State::AlreadyHere) ci->state = ContentInfo::State::Unselected;
	}
}

/**
 * Toggle the state of a content info and check its dependencies.
 * @param ci The content info to change.
 */
void ClientNetworkContentSocketHandler::ToggleSelectedState(const ContentInfo &ci)
{
	switch (ci.state) {
		case ContentInfo::State::Selected:
		case ContentInfo::State::Autoselected:
			this->Unselect(ci.id);
			break;

		case ContentInfo::State::Unselected:
			this->Select(ci.id);
			break;

		default:
			break;
	}
}

/**
 * Reverse lookup the dependencies of (direct) parents over a given child.
 * @param parents list to store all parents in (is not cleared)
 * @param child   the child to search the parents' dependencies for
 */
void ClientNetworkContentSocketHandler::ReverseLookupDependency(ConstContentVector &parents, const ContentInfo &child) const
{
	auto range = this->reverse_dependency_map.equal_range(child.id);

	for (auto iter = range.first; iter != range.second; ++iter) {
		parents.push_back(GetContent(iter->second));
	}
}

/**
 * Reverse lookup the dependencies of all parents over a given child.
 * @param tree  list to store all parents in (is not cleared)
 * @param child the child to search the parents' dependencies for
 */
void ClientNetworkContentSocketHandler::ReverseLookupTreeDependency(ConstContentVector &tree, const ContentInfo *child) const
{
	tree.push_back(child);

	/* First find all direct parents. We can't use the "normal" iterator as
	 * we are including stuff into the vector and as such the vector's data
	 * store can be reallocated (and thus move), which means our iterating
	 * pointer gets invalid. So fall back to the indices. */
	for (size_t i = 0; i < tree.size(); i++) {
		ConstContentVector parents;
		this->ReverseLookupDependency(parents, *tree[i]);

		for (const ContentInfo *ci : parents) {
			include(tree, ci);
		}
	}
}

/**
 * Check the dependencies (recursively) of this content info
 * @param ci the content info to check the dependencies of
 */
void ClientNetworkContentSocketHandler::CheckDependencyState(const ContentInfo &ci)
{
	if (ci.IsSelected() || ci.state == ContentInfo::State::AlreadyHere) {
		/* Selection is easy; just walk all children and set the
		 * autoselected state. That way we can see what we automatically
		 * selected and thus can unselect when a dependency is removed. */
		for (auto &dependency : ci.dependencies) {
			ContentInfo *c = this->GetContent(dependency);
			if (c == nullptr) {
				this->DownloadContentInfo(dependency);
			} else if (c->state == ContentInfo::State::Unselected) {
				c->state = ContentInfo::State::Autoselected;
				this->CheckDependencyState(*c);
			}
		}
		return;
	}

	if (ci.state != ContentInfo::State::Unselected) return;

	/* For unselection we need to find the parents of us. We need to
	 * unselect them. After that we unselect all children that we
	 * depend on and are not used as dependency for us, but only when
	 * we automatically selected them. */
	ConstContentVector parents;
	this->ReverseLookupDependency(parents, ci);
	for (const ContentInfo *c : parents) {
		if (!c->IsSelected()) continue;

		this->Unselect(c->id);
	}

	for (auto &dependency : ci.dependencies) {
		const ContentInfo *c = this->GetContent(dependency);
		if (c == nullptr) {
			DownloadContentInfo(dependency);
			continue;
		}
		if (c->state != ContentInfo::State::Autoselected) continue;

		/* Only unselect when WE are the only parent. */
		parents.clear();
		this->ReverseLookupDependency(parents, *c);

		/* First check whether anything depends on us */
		int sel_count = 0;
		bool force_selection = false;
		for (const ContentInfo *parent_ci : parents) {
			if (parent_ci->IsSelected()) sel_count++;
			if (parent_ci->state == ContentInfo::State::Selected) force_selection = true;
		}
		if (sel_count == 0) {
			/* Nothing depends on us */
			this->Unselect(c->id);
			continue;
		}
		/* Something manually selected depends directly on us */
		if (force_selection) continue;

		/* "Flood" search to find all items in the dependency graph*/
		parents.clear();
		this->ReverseLookupTreeDependency(parents, c);

		/* Is there anything that is "force" selected?, if so... we're done. */
		for (const ContentInfo *parent_ci : parents) {
			if (parent_ci->state != ContentInfo::State::Selected) continue;

			force_selection = true;
			break;
		}

		/* So something depended directly on us */
		if (force_selection) continue;

		/* Nothing depends on us, mark the whole graph as unselected.
		 * After that's done run over them once again to test their children
		 * to unselect. Don't do it immediately because it'll do exactly what
		 * we're doing now. */
		for (const ContentInfo *parent : parents) {
			if (parent->state == ContentInfo::State::Autoselected) this->Unselect(parent->id);
		}
		for (const ContentInfo *parent : parents) {
			this->CheckDependencyState(*this->GetContent(parent->id));
		}
	}
}

/** Clear all downloaded content information. */
void ClientNetworkContentSocketHandler::Clear()
{
	this->infos.clear();
	this->requested.clear();
	this->queued.clear();
	this->reverse_dependency_map.clear();
}

/*** CALLBACK ***/

void ClientNetworkContentSocketHandler::OnConnect(bool success)
{
	for (size_t i = 0; i < this->callbacks.size(); /* nothing */) {
		ContentCallback *cb = this->callbacks[i];
		/* the callback may remove itself from this->callbacks */
		cb->OnConnect(success);
		if (i != this->callbacks.size() && this->callbacks[i] == cb) i++;
	}
}

void ClientNetworkContentSocketHandler::OnDisconnect()
{
	for (size_t i = 0; i < this->callbacks.size(); /* nothing */) {
		ContentCallback *cb = this->callbacks[i];
		cb->OnDisconnect();
		if (i != this->callbacks.size() && this->callbacks[i] == cb) i++;
	}
}

void ClientNetworkContentSocketHandler::OnReceiveContentInfo(const ContentInfo &ci)
{
	for (size_t i = 0; i < this->callbacks.size(); /* nothing */) {
		ContentCallback *cb = this->callbacks[i];
		/* the callback may add items and/or remove itself from this->callbacks */
		cb->OnReceiveContentInfo(ci);
		if (i != this->callbacks.size() && this->callbacks[i] == cb) i++;
	}
}

void ClientNetworkContentSocketHandler::OnDownloadProgress(const ContentInfo &ci, int bytes)
{
	for (size_t i = 0; i < this->callbacks.size(); /* nothing */) {
		ContentCallback *cb = this->callbacks[i];
		cb->OnDownloadProgress(ci, bytes);
		if (i != this->callbacks.size() && this->callbacks[i] == cb) i++;
	}
}

void ClientNetworkContentSocketHandler::OnDownloadComplete(ContentID cid)
{
	ContentInfo *ci = this->GetContent(cid);
	if (ci != nullptr) {
		ci->state = ContentInfo::State::AlreadyHere;
	}

	for (size_t i = 0; i < this->callbacks.size(); /* nothing */) {
		ContentCallback *cb = this->callbacks[i];
		/* the callback may remove itself from this->callbacks */
		cb->OnDownloadComplete(cid);
		if (i != this->callbacks.size() && this->callbacks[i] == cb) i++;
	}
}
