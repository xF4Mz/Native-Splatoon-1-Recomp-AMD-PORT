// Accounts and online services, answered the way an offline console does:
// the user account works, the internet "connects" but nobody logs in, the
// friend list is empty, there are no amiibo on the reader, and sockets
// can't be opened. Games keep going after these answers; what they must not
// get is silence (an async request whose "done" callback never comes).
//
// Behaviour follows Cemu's offline mode, which is known to get Splatoon to
// its plaza: nn::ac says connected, nn::fp's login "succeeds" without
// logging in, and nn::act says the account has a friend account.
#include "online.h"
#include "guest_threads.h"
#include "sys.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <string.h>

static void ok(CPUState* c) { ret(c, 0); }
static void yes(CPUState* c) { ret(c, 1); }
static void put_string(u32 addr, const char* s) {
    if (!addr) return;
    for (u32 i = 0;; i++) { wiiu_write8(addr + i, (u8)s[i]); if (!s[i]) break; }
}
static void put_wstring(u32 addr, const char* s) {
    if (!addr) return;
    for (u32 i = 0;; i++) { wiiu_write16(addr + i * 2, (u16)(u8)s[i]); if (!s[i]) break; }
}

// --- nn::act (the user account; the basics are in sys.c) -------------------
static void act_uuid(CPUState* c) {   // (u8 uuid[16], slot, ...)
    static const u8 uuid[16] = { 0x53, 0x50, 0x4C, 0x54, 0x52, 0x45, 0x43, 0x4F,
                                 0x4D, 0x50, 0x00, 0x01, 0x80, 0x00, 0x00, 0x01 };
    u8* p = wiiu_mem_ptr(arg(c, 0), 16);
    if (p) memcpy(p, uuid, 16);
    ret(c, 0);
}
static void act_host_server_settings(CPUState* c) {   // (char* a, char* b, slot)
    put_string(arg(c, 0), "");
    put_string(arg(c, 1), "");
    ret(c, 0);
}
static void act_parental_slot(CPUState* c) {   // (u8* out, slot)
    if (arg(c, 0)) wiiu_write8(arg(c, 0), 1);
    ret(c, 0);
}
static void act_utc_offset(CPUState* c) { c->gpr[3] = 0; c->gpr[4] = 0; }   // s64 microseconds
static void act_utc_offset_ex(CPUState* c) {   // (s64* out, slot)
    if (arg(c, 0)) { wiiu_write32(arg(c, 0), 0); wiiu_write32(arg(c, 0) + 4, 0); }
    ret(c, 0);
}
static void act_account_id(CPUState* c) { put_string(arg(c, 0), "player"); ret(c, 0); }
int splat_online_local(void);
u32 splat_online_pid(void);
static void act_principal_id_ex(CPUState* c) {   // (u32* out, slot)
    if (arg(c, 0)) wiiu_write32(arg(c, 0), splat_online_local() ? splat_online_pid() : 0);
    ret(c, 0);
}
static void act_principal_id(CPUState* c) { ret(c, splat_online_local() ? splat_online_pid() : 0); }   // (0 offline, as before)
static void act_simple_address_ex(CPUState* c) {   // (u32* out, slot)
    if (arg(c, 0)) wiiu_write32(arg(c, 0), 0);
    ret(c, 0);
}
static void act_no_token(CPUState* c) { ret(c, 0xA0335300u); }   // no network account

// --- nn::acp: network time -------------------------------------------------
static void ACPGetNetworkTime(CPUState* c) {   // (s64* time, u32* valid)
    u64 t = wiiu_time_ticks();
    if (arg(c, 0)) { wiiu_write32(arg(c, 0), (u32)(t >> 32)); wiiu_write32(arg(c, 0) + 4, (u32)t); }
    if (arg(c, 1)) wiiu_write32(arg(c, 1), 1);
    ret(c, 0);
}

// --- nn::ac: the internet connection ---------------------------------------
static void ac_status_ok(CPUState* c) {   // (Status* out)
    if (arg(c, 0)) wiiu_write32(arg(c, 0), 0);
    ret(c, 0);
}
static void ac_is_connected(CPUState* c) {   // (bool* out)
    if (arg(c, 0)) wiiu_write8(arg(c, 0), 1);
    ret(c, 0);
}
static void ac_is_connected_u32(CPUState* c) {   // (u32* out)
    if (arg(c, 0)) wiiu_write32(arg(c, 0), 1);
    ret(c, 0);
}
static void ac_write_zero(CPUState* c) {   // (u32* out)
    if (arg(c, 0)) wiiu_write32(arg(c, 0), 0);
    ret(c, 0);
}
static void ac_config_existing(CPUState* c) {   // (id, bool* out)
    if (arg(c, 1)) wiiu_write8(arg(c, 1), 0);
    ret(c, 0);
}
static void ac_system_connected(CPUState* c) {   // (bool* connected, u32* type)
    if (arg(c, 0)) wiiu_write8(arg(c, 0), 1);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), 0);
    ret(c, 0);
}

// --- nn::fp: friends --------------------------------------------------------
static int g_fp_init = 0;
static void fp_initialize(CPUState* c) { g_fp_init++; ret(c, 0); }
static void fp_finalize(CPUState* c) { if (g_fp_init) g_fp_init--; ret(c, 0); }
static void fp_is_initialized(CPUState* c) { ret(c, g_fp_init > 0); }
static void fp_no(CPUState* c) { ret(c, 0); }
// Async calls: report "done, success" through the game's callback(result, context).
static void fp_async_0(CPUState* c) { threads_queue_callback(arg(c, 0), 0, arg(c, 1)); ret(c, 0); }
static void fp_async_1(CPUState* c) { threads_queue_callback(arg(c, 1), 0, arg(c, 2)); ret(c, 0); }
static void fp_async_2(CPUState* c) { threads_queue_callback(arg(c, 2), 0, arg(c, 3)); ret(c, 0); }
static void fp_async_3(CPUState* c) { threads_queue_callback(arg(c, 3), 0, arg(c, 4)); ret(c, 0); }
static void fp_list_empty(CPUState* c) {   // (u32* list, u32* count, start, max)
    if (arg(c, 1)) wiiu_write32(arg(c, 1), 0);
    ret(c, 0);
}
static void fp_my_account_id(CPUState* c) { put_string(arg(c, 0), "player"); ret(c, 0); }
static void fp_my_screen_name(CPUState* c) { put_wstring(arg(c, 0), "Player"); ret(c, 0); }
static void fp_my_mii(CPUState* c) { sys_write_default_mii(arg(c, 0)); ret(c, 0); }
static void fp_zero_struct(CPUState* c, u32 size) {
    u8* p = wiiu_mem_ptr(arg(c, 0), size);
    if (p) memset(p, 0, size);
    ret(c, 0);
}
static void fp_my_preference(CPUState* c) { fp_zero_struct(c, 4); }
static void fp_my_comment(CPUState* c) { fp_zero_struct(c, 0x24); }
static void fp_my_playing_game(CPUState* c) { fp_zero_struct(c, 0x10); }

// --- nn::nfp: amiibo (no figure on the reader) -----------------------------
static bool g_nfp_init = false, g_nfp_detecting = false;
static void nfp_initialize(CPUState* c) { g_nfp_init = true; g_nfp_detecting = false; ret(c, 0); }
static void nfp_finalize(CPUState* c) { g_nfp_init = false; ret(c, 0); }
static void nfp_start(CPUState* c) { g_nfp_detecting = true; ret(c, 0); }
static void nfp_stop(CPUState* c) { g_nfp_detecting = false; ret(c, 0); }
// 0 none, 1 initialized, 2 searching
static void nfp_state(CPUState* c) { ret(c, !g_nfp_init ? 0 : g_nfp_detecting ? 2 : 1); }
static void nfp_no_tag(CPUState* c) { ret(c, 0xA1B10400u); }   // "no tag" style failure

// --- nsysnet / nlibcurl: no sockets (the sockets themselves: net_offline.c, offline) ----------------------------------------
static void net_fail(CPUState* c) { ret(c, (u32)-1); }
static void net_null(CPUState* c) { ret(c, 0); }

static const HleEntry g_online_table[] = {
    { "GetUuid__Q2_2nn3actFP7ACTUuid", act_uuid },
    { "GetUuidEx__Q2_2nn3actFP7ACTUuidUc", act_uuid },
    { "GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi", act_uuid },
    { "GetHostServerSettings__Q2_2nn3actFPcT1Uc", act_host_server_settings },
    { "GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc", act_parental_slot },
    { "GetParentalControlSlotNo__Q2_2nn3actFv", yes },
    { "GetUtcOffset__Q2_2nn3actFv", act_utc_offset },
    { "GetUtcOffsetEx__Q2_2nn3actFPLUc", act_utc_offset_ex },
    { "GetAccountId__Q2_2nn3actFPc", act_account_id },
    { "GetAccountIdEx__Q2_2nn3actFPcUc", act_account_id },
    { "GetPrincipalId__Q2_2nn3actFv", act_principal_id },
    { "GetPrincipalIdEx__Q2_2nn3actFPUiUc", act_principal_id_ex },
    { "GetSimpleAddressId__Q2_2nn3actFv", ok },
    { "GetSimpleAddressIdEx__Q2_2nn3actFPUiUc", act_simple_address_ex },
    { "IsCommitted__Q2_2nn3actFv", yes },
    { "IsServerAccountActive__Q2_2nn3actFv", ok },
    { "IsPasswordCacheEnabled__Q2_2nn3actFv", ok },
    { "IsParentalControlCheckEnabled__Q2_2nn3actFv", ok },
    { "AcquireIndependentServiceToken__Q2_2nn3actFPcPCc", act_no_token },
    { "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUi", act_no_token },
    { "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4", act_no_token },

    { "ACPGetNetworkTime", ACPGetNetworkTime },

    { "Initialize__Q2_2nn2acFv", ok },
    { "Finalize__Q2_2nn2acFv", ok },
    { "Connect__Q2_2nn2acFv", ok },
    { "ConnectAsync__Q2_2nn2acFv", ok },
    { "Close__Q2_2nn2acFv", ok },
    { "GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status", ac_status_ok },
    { "GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status", ac_status_ok },
    { "GetConnectResult__Q2_2nn2acFPQ2_2nn6Result", ac_write_zero },
    { "GetLastErrorCode__Q2_2nn2acFPUi", ac_write_zero },
    { "IsApplicationConnected__Q2_2nn2acFPb", ac_is_connected },
    { "IsConfigExisting__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumPb", ac_config_existing },
    { "GetStartupId__Q2_2nn2acFPQ3_2nn2ac11ConfigIdNum", ac_write_zero },
    { "ACInitialize", ok },
    { "ACFinalize", ok },
    { "ACConnect", ok },
    { "ACConnectAsync", ok },
    { "ACClose", ok },
    { "ACGetConnectStatus", ac_status_ok },
    { "ACGetStatus", ac_status_ok },
    { "ACIsApplicationConnected", ac_is_connected_u32 },
    { "ACGetLastErrorCode", ac_write_zero },
    { "ACIsSystemConnected", ac_system_connected },
    { "ACGetStartupId", ac_write_zero },

    { "Initialize__Q2_2nn2fpFv", fp_initialize },
    { "InitializeAdmin__Q2_2nn2fpFv", fp_initialize },
    { "Finalize__Q2_2nn2fpFv", fp_finalize },
    { "FinalizeAdmin__Q2_2nn2fpFv", fp_finalize },
    { "IsInitialized__Q2_2nn2fpFv", fp_is_initialized },
    { "IsInitializedAdmin__Q2_2nn2fpFv", fp_is_initialized },
    { "SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv", ok },
    { "LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", fp_async_0 },
    { "HasLoggedIn__Q2_2nn2fpFv", fp_no },
    { "IsOnline__Q2_2nn2fpFv", fp_no },
    { "GetFriendList__Q2_2nn2fpFPUiT1UiT3", fp_list_empty },
    { "GetFriendListAll__Q2_2nn2fpFPUiT1UiT3", fp_list_empty },
    { "GetFriendRequestList__Q2_2nn2fpFPUiT1UiT3", fp_list_empty },
    { "GetBlackList__Q2_2nn2fpFPUiT1UiT3", fp_list_empty },
    { "GetBasicInfoAsync__Q2_2nn2fpFPQ3_2nn2fp9BasicInfoPCUiUiPFQ2_2nn6ResultPv_vPv", fp_async_3 },
    { "GetMyPrincipalId__Q2_2nn2fpFv", fp_no },
    { "GetMyAccountId__Q2_2nn2fpFPc", fp_my_account_id },
    { "GetMyScreenName__Q2_2nn2fpFPw", fp_my_screen_name },
    { "GetMyMii__Q2_2nn2fpFP12FFLStoreData", fp_my_mii },
    { "GetMyPreference__Q2_2nn2fpFPQ3_2nn2fp10Preference", fp_my_preference },
    { "GetMyComment__Q2_2nn2fpFPQ3_2nn2fp7Comment", fp_my_comment },
    { "GetMyPlayingGame__Q2_2nn2fpFPQ3_2nn2fp7GameKey", fp_my_playing_game },
    { "IsPreferenceValid__Q2_2nn2fpFv", yes },
    { "CheckSettingStatusAsync__Q2_2nn2fpFPUcPFQ2_2nn6ResultPv_vPv", fp_async_1 },
    { "UpdateCommentAsync__Q2_2nn2fpFPCwPFQ2_2nn6ResultPv_vPv", fp_async_1 },
    { "UpdatePreferenceAsync__Q2_2nn2fpFPCQ3_2nn2fp10PreferencePFQ2_2nn6ResultPv_vPv", fp_async_1 },
    { "GetRequestBlockSettingAsync__Q2_2nn2fpFPUcPCUiUiPFQ2_2nn6ResultPv_vPv", fp_async_3 },
    { "AddFriendAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv", fp_async_1 },
    { "RemoveFriendAsync__Q2_2nn2fpFUiPFQ2_2nn6ResultPv_vPv", fp_async_1 },
    { "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw", ok },
    { "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCwUi", ok },
    { "RegisterAccount__Q2_2nn2fpFv", ok },
    { "RegisterAccountAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", fp_async_0 },
    { "DeleteFriendFlagsAsync__Q2_2nn2fpFPCUiUiT2PFQ2_2nn6ResultPv_vPv", fp_async_2 },

    { "Initialize__Q2_2nn3nfpFv", nfp_initialize },
    { "Finalize__Q2_2nn3nfpFv", nfp_finalize },
    { "GetNfpState__Q2_2nn3nfpFv", nfp_state },
    { "StartDetection__Q2_2nn3nfpFv", nfp_start },
    { "StopDetection__Q2_2nn3nfpFv", nfp_stop },
    { "SetActivateEvent__Q2_2nn3nfpFP7OSEvent", ok },
    { "SetDeactivateEvent__Q2_2nn3nfpFP7OSEvent", ok },
    { "Mount__Q2_2nn3nfpFv", nfp_no_tag },
    { "MountRom__Q2_2nn3nfpFv", nfp_no_tag },
    { "Unmount__Q2_2nn3nfpFv", ok },
    { "GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo", nfp_no_tag },

    { "EnableResumeDaemons__Q2_2nn3ndmFv", ok },
    { "SuspendDaemons__Q2_2nn3ndmFUi", ok },
    { "ResumeDaemons__Q2_2nn3ndmFUi", ok },
    { "IsDaemonStartupCompleted__Q2_2nn3ndmFv", yes },
    { "Initialize__Q2_2nn3ndmFv", ok },
    { "Finalize__Q2_2nn3ndmFv", ok },

    { "socket_lib_finish", ok },
    { "set_resolver_allocator", ok },
    { "NSSLInit", ok },
    { "NSSLFinish", ok },
    { "NSSLCreateContext", net_fail },
    { "curl_global_init", ok },
    { "curl_easy_init", net_null },
};

const HleEntry* online_hle_table(size_t* count) {
    *count = sizeof(g_online_table) / sizeof(g_online_table[0]);
    return g_online_table;
}
