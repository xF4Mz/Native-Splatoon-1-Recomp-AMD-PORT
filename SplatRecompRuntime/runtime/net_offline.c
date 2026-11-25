// Offline answers for the console's network (sockets, nn::ac, nn::act) and BOSS download calls.
// Online play isn't part of this release: every call gets the answer the game gets with no network.
#include "hle.h"
#include "online.h"

bool splat_online_on(void) { return false; }
u32 splat_online_pid(void) { return 0; }
int splat_online_local(void) { return 0; }

static void net_ret_m1(CPUState* c) { ret(c, (u32)(-1)); }
static void net_ret_0(CPUState* c) { ret(c, (u32)(0)); }
static void boss_keep(CPUState* c) { (void)c; }   // (constructors: hand the object back)
static void boss_zero(CPUState* c) { ret(c, 0); }

static const HleEntry g_net_table[] = {
    { "socket_lib_init", net_ret_0 },
    { "socket", net_ret_m1 },
    { "mw_socket", net_ret_0 },
    { "socketclose", net_ret_0 },
    { "shutdown", net_ret_0 },
    { "bind", net_ret_m1 },
    { "connect", net_ret_m1 },
    { "listen", net_ret_m1 },
    { "accept", net_ret_m1 },
    { "send", net_ret_m1 },
    { "sendto", net_ret_m1 },
    { "sendto_multi", net_ret_0 },
    { "recv", net_ret_m1 },
    { "recvfrom", net_ret_m1 },
    { "recvfrom_ex", net_ret_0 },
    { "setsockopt", net_ret_m1 },
    { "getsockopt", net_ret_m1 },
    { "getsockname", net_ret_0 },
    { "select", net_ret_0 },
    { "socketlasterr", net_ret_0 },
    { "gethostbyname", net_ret_0 },
    { "getaddrinfo", net_ret_m1 },
    { "freeaddrinfo", net_ret_0 },
    { "inet_ntoa", net_ret_0 },
    { "inet_pton", net_ret_0 },
    { "htonl", net_ret_0 },
    { "ntohl", net_ret_0 },
    { "htons", net_ret_0 },
    { "ntohs", net_ret_0 },
    { "GetAssignedAddress__Q2_2nn2acFPUl", net_ret_0 },
    { "GetAssignedSubnet__Q2_2nn2acFPUl", net_ret_0 },
    { "AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi", net_ret_0 },
};
static const HleEntry g_boss_table[] = {
    { "Initialize__Q2_2nn4bossFv", boss_zero },
    { "__ct__Q3_2nn4boss4TaskFv", boss_keep },
    { "__dt__Q3_2nn4boss4TaskFv", boss_zero },
    { "Initialize__Q3_2nn4boss4TaskFPCc", boss_zero },
    { "IsRegistered__Q3_2nn4boss4TaskCFv", boss_zero },
    { "Register__Q3_2nn4boss4TaskFRQ3_2nn4boss11TaskSetting", boss_zero },
    { "RegisterForImmediateRun__Q3_2nn4boss4TaskFRCQ3_2nn4boss11TaskSetting", boss_zero },
    { "Unregister__Q3_2nn4boss4TaskFv", boss_zero },
    { "StopScheduling__Q3_2nn4boss4TaskFv", boss_zero },
    { "Run__Q3_2nn4boss4TaskFb", boss_zero },
    { "Wait__Q3_2nn4boss4TaskFUiQ3_2nn4boss13TaskWaitState", boss_zero },
    { "GetTurnState__Q3_2nn4boss4TaskCFPUi", boss_zero },
    { "GetHttpStatusCode__Q3_2nn4boss4TaskCFPUi", boss_zero },
    { "GetContentLength__Q3_2nn4boss4TaskCFPUi", boss_zero },
    { "GetProcessedLength__Q3_2nn4boss4TaskCFPUi", boss_zero },
    { "GetResult__Q3_2nn4boss4TaskCFPUi", boss_zero },
    { "__ct__Q3_2nn4boss10TaskResultFQ2_2nn6Result", boss_keep },
    { "ErrorCode__Q3_2nn4boss10TaskResultCFv", boss_zero },
    { "__ct__Q3_2nn4boss15NbdlTaskSettingFv", boss_keep },
    { "__dt__Q3_2nn4boss15NbdlTaskSettingFv", boss_zero },
    { "Initialize__Q3_2nn4boss15NbdlTaskSettingFPCcLT1", boss_zero },
    { "AddInternalCaCert__Q3_2nn4boss14NetTaskSettingFSc", boss_zero },
    { "SetInternalClientCert__Q3_2nn4boss14NetTaskSettingFSc", boss_zero },
    { "__ct__Q3_2nn4boss16RawUlTaskSettingFv", boss_keep },
    { "__dt__Q3_2nn4boss16RawUlTaskSettingFv", boss_zero },
    { "Initialize__Q3_2nn4boss16RawUlTaskSettingFPCcPCUcL", boss_zero },
    { "AddLargeHttpHeader__Q3_2nn4boss16RawUlTaskSettingFPCcT1", boss_zero },
    { "__ct__Q3_2nn4boss17PlayReportSettingFv", boss_keep },
    { "__dt__Q3_2nn4boss17PlayReportSettingFv", boss_zero },
    { "Initialize__Q3_2nn4boss17PlayReportSettingFPvUi", boss_zero },
    { "Set__Q3_2nn4boss17PlayReportSettingFPCcUi", boss_zero },
    { "__ct__Q3_2nn4boss5TitleFv", boss_keep },
    { "__dt__Q3_2nn4boss5TitleFv", boss_zero },
    { "SetNewArrivalFlagOff__Q3_2nn4boss5TitleFv", boss_zero },
    { "__ct__Q3_2nn4boss7StorageFv", boss_keep },
    { "__dt__Q3_2nn4boss7StorageFv", boss_zero },
    { "Initialize__Q3_2nn4boss7StorageFPCcQ3_2nn4boss11StorageKind", boss_zero },
    { "Exist__Q3_2nn4boss7StorageCFv", boss_zero },
    { "GetDataList__Q3_2nn4boss7StorageCFPQ3_2nn4boss8DataNameUiPUiT2", boss_zero },
    { "__ct__Q3_2nn4boss8DataNameFv", boss_keep },
    { "__ct__Q3_2nn4boss6NsDataFv", boss_keep },
    { "__dt__Q3_2nn4boss6NsDataFv", boss_zero },
    { "Initialize__Q3_2nn4boss6NsDataFRCQ3_2nn4boss7StoragePCc", boss_zero },
    { "Exist__Q3_2nn4boss6NsDataCFv", boss_zero },
    { "GetSize__Q3_2nn4boss6NsDataCFv", boss_zero },
    { "Read__Q3_2nn4boss6NsDataFPvUi", boss_zero },
};

const HleEntry* online_net_hle_table(size_t* count) { *count = sizeof(g_net_table) / sizeof(g_net_table[0]); return g_net_table; }
const HleEntry* boss_hle_table(size_t* count) { *count = sizeof(g_boss_table) / sizeof(g_boss_table[0]); return g_boss_table; }
