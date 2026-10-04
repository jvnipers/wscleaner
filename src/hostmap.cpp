#include <stdio.h>
#include <time.h>
#include <string>
#include "hostmap.h"
#include "workshop_manager.h"
#include "icommandline.h"
#include "engine/igameeventsystem.h"
#include "networksystem/inetworkmessages.h"
#include "networksystem/netmessage.h"
#include "usermessages.pb.h"

WSHostMapPlugin g_ThisPlugin;
PLUGIN_EXPOSE(WSHostMapPlugin, g_ThisPlugin);

static CSteamGameServerAPIContext s_SteamAPI;
static IGameEventSystem *s_pGameEventSystem = nullptr;

CConVar<float> wshostmap_progress_interval("wshostmap_progress_interval", FCVAR_NONE, "Seconds between download progress messages in chat.", 5.0f, true, 1.0f, false, 0.0f);

enum class EHostState
{
	Idle,
	Querying,    // waiting on the workshop details query
	Downloading, // host_workshop_map issued, waiting for the workshop manager to finish
};

static struct
{
	EHostState state = EHostState::Idle;
	PublishedFileId_t fileId = 0;
	UGCQueryHandle_t hQuery = k_UGCQueryHandleInvalid;
	SteamAPICall_t hCall = k_uAPICallInvalid;
	std::string title;
	double startTime = 0.0;
	double nextReportTime = 0.0;
} s_Request;

// Give up on a download the workshop manager cannot be watched for. Only used when the manager was not found.
static const double kUnwatchedTimeout = 600.0;

static void ResetRequest()
{
	if (s_Request.hQuery != k_UGCQueryHandleInvalid && s_SteamAPI.SteamUGC())
		s_SteamAPI.SteamUGC()->ReleaseQueryUGCRequest(s_Request.hQuery);
	s_Request.state = EHostState::Idle;
	s_Request.fileId = 0;
	s_Request.hQuery = k_UGCQueryHandleInvalid;
	s_Request.hCall = k_uAPICallInvalid;
	s_Request.title.clear();
}

// Print to every player's chat as a plain TextMsg, prefixed with a green tag. Control characters are dropped so
// workshop titles and player names cannot inject chat colours.
static void ChatPrintAll(const char *format, ...)
{
	char message[256];
	va_list args;
	va_start(args, format);
	V_vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	// The leading space is needed for a colour code at the very start to be parsed.
	std::string text = " \x04[HostMap]\x01 ";
	for (const char *c = message; *c; ++c)
	{
		if ((unsigned char)*c >= ' ')
			text += *c;
	}

	INetworkMessageInternal *pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	if (!pNetMsg)
		return;
	CNetMessagePB<CUserMessageTextMsg> *pMsg = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	pMsg->set_dest(3); // HUD_PRINTTALK
	pMsg->add_param(text);
	// A client count of -1 with no client mask posts to every connected client.
	s_pGameEventSystem->PostEventAbstract(-1, false, -1, nullptr, pNetMsg, pMsg, 0, BUF_RELIABLE);
	delete pMsg;
}

static std::string FormatSize(uint64 bytes)
{
	char buffer[32];
	if (bytes >= 1024ull * 1024 * 1024)
		V_snprintf(buffer, sizeof(buffer), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
	else
		V_snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
	return buffer;
}

static std::string FormatDate(uint32 unixTime)
{
	time_t time = (time_t)unixTime;
	struct tm utc;
#ifdef _WIN32
	gmtime_s(&utc, &time);
#else
	gmtime_r(&time, &utc);
#endif
	char buffer[32];
	strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &utc);
	return buffer;
}

// Accepts a bare id or a workshop URL containing "id=<id>".
static PublishedFileId_t ParseFileId(const char *text)
{
	while (*text == ' ')
		++text;
	const char *idParam = V_strstr(text, "id=");
	if (idParam)
		text = idParam + 3;
	return strtoull(text, nullptr, 10);
}

static void StartRequest(CPlayerSlot slot, const char *arguments)
{
	if (s_Request.state != EHostState::Idle)
	{
		ChatPrintAll("Already working on workshop map %llu, try again once it is loaded.", s_Request.fileId);
		return;
	}
	PublishedFileId_t fileId = ParseFileId(arguments);
	if (fileId == 0)
	{
		ChatPrintAll("Usage: !hostmap <workshop id>");
		return;
	}
	ISteamUGC *pUGC = s_SteamAPI.SteamUGC();
	if (!pUGC)
	{
		ChatPrintAll("Steam is not available yet, try again in a moment.");
		return;
	}

	UGCQueryHandle_t hQuery = pUGC->CreateQueryUGCDetailsRequest(&fileId, 1);
	SteamAPICall_t hCall = hQuery != k_UGCQueryHandleInvalid ? pUGC->SendQueryUGCRequest(hQuery) : k_uAPICallInvalid;
	if (hCall == k_uAPICallInvalid)
	{
		if (hQuery != k_UGCQueryHandleInvalid)
			pUGC->ReleaseQueryUGCRequest(hQuery);
		ChatPrintAll("Could not query workshop item %llu.", fileId);
		return;
	}
	s_Request.state = EHostState::Querying;
	s_Request.fileId = fileId;
	s_Request.hQuery = hQuery;
	s_Request.hCall = hCall;

	const char *playerName = slot.Get() >= 0 ? g_pEngineServer->GetClientConVarValue(slot, "name") : nullptr;
	ChatPrintAll("%s requested workshop map %llu, looking it up...", playerName && *playerName ? playerName : "Console", fileId);
}

static void UpdateQuery()
{
	ISteamUtils *pUtils = s_SteamAPI.SteamGameServerUtils();
	ISteamUGC *pUGC = s_SteamAPI.SteamUGC();
	if (!pUtils || !pUGC)
		return;
	bool bFailed = false;
	if (!pUtils->IsAPICallCompleted(s_Request.hCall, &bFailed))
		return;

	SteamUGCQueryCompleted_t result = {};
	SteamUGCDetails_t details = {};
	bool bOk = !bFailed
		&& pUtils->GetAPICallResult(s_Request.hCall, &result, sizeof(result), SteamUGCQueryCompleted_t::k_iCallback, &bFailed)
		&& !bFailed
		&& result.m_eResult == k_EResultOK
		&& result.m_unNumResultsReturned > 0
		&& pUGC->GetQueryUGCResult(s_Request.hQuery, 0, &details)
		&& details.m_eResult == k_EResultOK;
	PublishedFileId_t fileId = s_Request.fileId;
	pUGC->ReleaseQueryUGCRequest(s_Request.hQuery);
	s_Request.hQuery = k_UGCQueryHandleInvalid;

	if (!bOk)
	{
		ChatPrintAll("Workshop item %llu was not found.", fileId);
		ResetRequest();
		return;
	}
	if (details.m_nConsumerAppID != 730 || details.m_eFileType == k_EWorkshopFileTypeCollection || details.m_bBanned)
	{
		ChatPrintAll("Workshop item %llu is not a CS2 map.", fileId);
		ResetRequest();
		return;
	}

	s_Request.title = details.m_rgchTitle;
	uint64 size = details.m_ulTotalFilesSize ? details.m_ulTotalFilesSize : (uint64)(details.m_nFileSize > 0 ? details.m_nFileSize : 0);
	ChatPrintAll("%s (%llu): last updated %s, size %s.", s_Request.title.c_str(), fileId,
		FormatDate(details.m_rtimeUpdated).c_str(), size ? FormatSize(size).c_str() : "unknown");

	// The workshop manager downloads the item and changes to it once it is installed.
	char command[64];
	V_snprintf(command, sizeof(command), "host_workshop_map %llu\n", fileId);
	g_pEngineServer->ServerCommand(command);

	s_Request.state = EHostState::Downloading;
	s_Request.startTime = Plat_FloatTime();
	s_Request.nextReportTime = s_Request.startTime + wshostmap_progress_interval.Get();
}

static void UpdateDownload()
{
	ISteamUGC *pUGC = s_SteamAPI.SteamUGC();
	if (!pUGC)
		return;
	double now = Plat_FloatTime();
	bool bReportDue = now >= s_Request.nextReportTime;

	// The manager drops the request in the frame it finishes and changes map right away, so check that every frame.
	// host_workshop_map only runs a frame after it is queued, hence the short grace period.
	bool bWatched = IsWorkshopManagerAvailable();
	if (!bWatched && !bReportDue)
		return;
	if (bWatched && (IsWorkshopManagerRequestPending(s_Request.fileId) || now - s_Request.startTime < 1.0) && !bReportDue)
		return;

	uint32 itemState = pUGC->GetItemState(s_Request.fileId);
	bool bDownloading = (itemState & (k_EItemStateDownloading | k_EItemStateDownloadPending)) != 0;
	bool bInstalled = (itemState & k_EItemStateInstalled) != 0 && !bDownloading;
	bool bDone = bWatched
		? !IsWorkshopManagerRequestPending(s_Request.fileId) && now - s_Request.startTime >= 1.0
		: bInstalled || now - s_Request.startTime > kUnwatchedTimeout;
	if (bDone)
	{
		if (bInstalled)
			ChatPrintAll("%s is ready, changing map.", s_Request.title.c_str());
		else
			ChatPrintAll("Failed to download %s.", s_Request.title.c_str());
		ResetRequest();
		return;
	}
	if (!bReportDue)
		return;
	s_Request.nextReportTime = now + wshostmap_progress_interval.Get();

	uint64 downloaded = 0, total = 0;
	if (bDownloading && pUGC->GetItemDownloadInfo(s_Request.fileId, &downloaded, &total) && total > 0)
	{
		ChatPrintAll("Downloading %s: %d%% (%s / %s)", s_Request.title.c_str(), (int)(downloaded * 100 / total),
			FormatSize(downloaded).c_str(), FormatSize(total).c_str());
	}
}

WSHostMapPlugin::WSHostMapPlugin() :
	m_GameServerSteamAPIActivated(&IServerGameDLL::GameServerSteamAPIActivated, this, &WSHostMapPlugin::Hook_GameServerSteamAPIActivated, nullptr),
	m_GameFrame(&IServerGameDLL::GameFrame, this, nullptr, &WSHostMapPlugin::Hook_GameFrame),
	m_DispatchConCommand(&ICvar::DispatchConCommand, this, &WSHostMapPlugin::Hook_DispatchConCommand, nullptr)
{
}

bool WSHostMapPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	if (!CommandLine()->CheckParm("-dedicated"))
	{
		snprintf(error, maxlen, "This plugin can only be run on dedicated servers.");
		return false;
	}
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, s_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	g_SMAPI->AddListener(this, this);
	if (late)
		s_SteamAPI.Init();
	ConVar_Register();

	m_GameServerSteamAPIActivated.Add(g_pSource2Server);
	m_GameFrame.Add(g_pSource2Server);
	m_DispatchConCommand.Add(g_pCVar);
	return true;
}

bool WSHostMapPlugin::Unload(char *error, size_t maxlen)
{
	m_GameServerSteamAPIActivated.Remove(g_pSource2Server);
	m_GameFrame.Remove(g_pSource2Server);
	m_DispatchConCommand.Remove(g_pCVar);
	ResetRequest();
	return true;
}

void WSHostMapPlugin::AllPluginsLoaded()
{
	g_pEngineServer->ServerCommand("exec wshostmap/wshostmap");
}

KHook::Return<void> WSHostMapPlugin::Hook_GameServerSteamAPIActivated(IServerGameDLL*)
{
	if (!s_SteamAPI.SteamUGC())
		s_SteamAPI.Init();
	return { KHook::Action::Ignore };
}

KHook::Return<void> WSHostMapPlugin::Hook_GameFrame(IServerGameDLL*, bool simulating, bool bFirstTick, bool bLastTick)
{
	if (s_Request.state == EHostState::Querying)
		UpdateQuery();
	else if (s_Request.state == EHostState::Downloading)
		UpdateDownload();
	return { KHook::Action::Ignore };
}

KHook::Return<void> WSHostMapPlugin::Hook_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args)
{
	const char *name = cmd.GetName();
	if (args.ArgC() < 2 || (V_stricmp(name, "say") != 0 && V_stricmp(name, "say_team") != 0))
		return { KHook::Action::Ignore };

	// The chat message arrives as one argument; the console's own say has it split over several.
	const char *message = args.ArgS();
	if (*message == '"')
		++message;
	static const char kTrigger[] = "!hostmap";
	if (V_strnicmp(message, kTrigger, sizeof(kTrigger) - 1) != 0)
		return { KHook::Action::Ignore };
	const char *arguments = message + sizeof(kTrigger) - 1;
	if (*arguments != '\0' && *arguments != ' ' && *arguments != '"')
		return { KHook::Action::Ignore };

	StartRequest(ctx.GetPlayerSlot(), arguments);
	return { KHook::Action::Ignore };
}
