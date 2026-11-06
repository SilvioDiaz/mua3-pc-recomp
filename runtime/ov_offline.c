// Offline answers for the network SDK libraries MUA3 links: nifm, socket, ldn, bcat, friends, nsd, ssl and ec, plus
// the ErrorResultVariant form of err::ShowError. None of them talks to a Horizon service. The game sees a Switch with
// no connection: nifm reports no network, every socket call fails with ENETDOWN, local wireless (ldn) and BCAT
// delivery fail with a nonzero Result, and the friend list is empty. Each function logs its first call with the
// caller, so a run shows what the game tried.
//
// Functions that never leave the process (InetHtons, MakeIpv4Address, ErrorResultVariant's constructors and
// operators, Friend's accessors, the DeliveryCacheFile and DeliveryCacheProgress constructors) stay lifted.
// Results are Horizon's module | description << 9; the descriptions are not the SDK's own codes, only nonzero, so a
// game that checks IsSuccess() takes its offline path.
#include <stdint.h>

#include "host.h"
#include "platform/platform.h"

#define RESULT(module, desc) ((uint64_t)(module) | (uint64_t)(desc) << 9)
#define R_NIFM_OFFLINE RESULT(110, 1)
#define R_LDN_UNAVAILABLE RESULT(203, 1)
#define R_BCAT_UNAVAILABLE RESULT(122, 1)
#define R_FRIENDS_UNAVAILABLE RESULT(121, 1)
#define R_NSD_UNAVAILABLE RESULT(141, 1)
#define R_SSL_UNAVAILABLE RESULT(123, 1)
#define SOCKET_ENETDOWN 50   // BSD errno, as nn::socket reports it
#define SOCKET_EAI_FAIL 4    // BSD EAI_FAIL

static void first_call(Cpu *c, volatile int32_t *seen, const char *what) {
    if (!plat_xchg32(seen, 1)) {
        char at[128];
        rt_log("offline: %s from %s (answered: no network)", what, ldr_describe(c->x[30] - 4, at, sizeof at));
    }
}

// OFFLINE(fn, "nn::x::Y", value): a host function that logs its first call and returns value in x0.
#define OFFLINE(fn, what, value)                    \
    static void fn(Cpu *c) {                        \
        static volatile int32_t seen;               \
        first_call(c, &seen, what);                 \
        ret_x(c, (uint64_t)(value));                \
    }

// ---- nifm: no network; requests are accepted and never succeed
OFFLINE(nifm_initialize, "nn::nifm::Initialize", 0)
OFFLINE(nifm_available, "nn::nifm::IsNetworkAvailable", 0)
OFFLINE(nifm_submit, "nn::nifm::SubmitNetworkRequest", 0)
OFFLINE(nifm_cancel, "nn::nifm::CancelNetworkRequest", 0)
OFFLINE(nifm_on_hold, "nn::nifm::IsNetworkRequestOnHold", 0)
OFFLINE(nifm_proxy, "nn::nifm::GetCurrentProxySetting", R_NIFM_OFFLINE)
OFFLINE(nifm_ip, "nn::nifm::GetCurrentPrimaryIpAddress", R_NIFM_OFFLINE)
OFFLINE(nifm_handle_error, "nn::nifm::HandleNetworkRequestErrorResult", R_NIFM_OFFLINE)

// ---- socket: the library initialises; every socket call fails with ENETDOWN
OFFLINE(socket_initialize, "nn::socket::Initialize", 0)
OFFLINE(socket_finalize, "nn::socket::Finalize", 0)
OFFLINE(socket_fail, "nn::socket call", -1)
OFFLINE(socket_errno, "nn::socket::GetLastErrno", SOCKET_ENETDOWN)
OFFLINE(socket_gai, "nn::socket::GetAddrInfo", SOCKET_EAI_FAIL)
OFFLINE(socket_free_ai, "nn::socket::FreeAddrInfo", 0)

// ---- ldn (local wireless): not available
OFFLINE(ldn_fail, "nn::ldn call", R_LDN_UNAVAILABLE)
OFFLINE(ldn_void, "nn::ldn::Finalize/Close", 0)
OFFLINE(ldn_state, "nn::ldn::GetState", 0)   // State::None

// ---- bcat (data delivery): no delivery cache
OFFLINE(bcat_initialize, "nn::bcat::Initialize", 0)
OFFLINE(bcat_fail, "nn::bcat call", R_BCAT_UNAVAILABLE)
OFFLINE(bcat_void, "nn::bcat::Unmount/Close/Update", 0)
OFFLINE(bcat_status, "nn::bcat::DeliveryCacheProgress::GetStatus", 9)   // DeliveryCacheProgressStatus::Done

// ---- friends: empty friend list, no play history
OFFLINE(friends_initialize, "nn::friends::Initialize", 0)
OFFLINE(friends_fail, "nn::friends call", R_FRIENDS_UNAVAILABLE)

static void friends_list(Cpu *c) {   // Result GetFriendList(int *count, Friend *out, int offset, int max, filter)
    static volatile int32_t seen;
    first_call(c, &seen, "nn::friends::GetFriendList");
    int32_t *count = arg_p(c, 0);
    if (count) *count = 0;
    ret_x(c, R_FRIENDS_UNAVAILABLE);
}

// ---- nsd, ssl, ec, err
OFFLINE(nsd_resolve, "nn::nsd::ResolveEx", R_NSD_UNAVAILABLE)
OFFLINE(ssl_fail, "nn::ssl::Context call", R_SSL_UNAVAILABLE)
OFFLINE(ec_shop, "nn::ec::ShowShopAddOnContentList (eShop not available)", 0)
OFFLINE(err_show_variant, "nn::err::ShowError(ErrorResultVariant) (error applet not shown)", 0)
OFFLINE(err_multimedia, "nn::err::ReportMultimediaError", 0)

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", nifm_initialize);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available);
HOST_OVERRIDE("_ZN2nn4nifm20SubmitNetworkRequestEv", nifm_submit);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel);
HOST_OVERRIDE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", nifm_on_hold);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_proxy);
HOST_OVERRIDE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_ip);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_handle_error);

HOST_OVERRIDE("_ZN2nn6socket10InitializeEPvmmi", socket_initialize);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", socket_finalize);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrnoEv", socket_errno);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", socket_gai);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", socket_gai);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", socket_free_ai);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", socket_free_ai);
HOST_OVERRIDE("_ZN2nn6socket10GetSockOptEiiiPvPj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiiiPKvj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPK8sockaddrj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket4PollEP6pollfdmi", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket4RecvEiPvmi", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket4SendEiPKvmi", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket5CloseEi", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket5FcntlEiiz", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket6AcceptEiP8sockaddrPj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket6ListenEii", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket6SocketEiii", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket7ConnectEiPK8sockaddrj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", socket_fail);
HOST_OVERRIDE("_ZN2nn6socket8ShutdownEii", socket_fail);

HOST_OVERRIDE("_ZN2nn3ldn10InitializeEv", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn8FinalizeEv", ldn_void);
HOST_OVERRIDE("_ZN2nn3ldn8GetStateEv", ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn19GetDisconnectReasonEv", ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn10DisconnectEv", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn11OpenStationEv", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn12CloseStationEv", ldn_void);
HOST_OVERRIDE("_ZN2nn3ldn15OpenAccessPointEv", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn16CloseAccessPointEv", ldn_void);
HOST_OVERRIDE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn14DestroyNetworkEv", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_fail);
HOST_OVERRIDE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_fail);

HOST_OVERRIDE("_ZN2nn4bcat10InitializeEv", bcat_initialize);
HOST_OVERRIDE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", bcat_fail);
HOST_OVERRIDE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", bcat_void);
HOST_OVERRIDE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", bcat_fail);
HOST_OVERRIDE("_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", bcat_void);
HOST_OVERRIDE("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv", bcat_status);
HOST_OVERRIDE("_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv", bcat_fail);
HOST_OVERRIDE("_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", bcat_fail);
HOST_OVERRIDE("_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", bcat_fail);
HOST_OVERRIDE("_ZN2nn4bcat17DeliveryCacheFile5CloseEv", bcat_void);

HOST_OVERRIDE("_ZN2nn7friends10InitializeEv", friends_initialize);
HOST_OVERRIDE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", friends_list);
HOST_OVERRIDE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friends_fail);
HOST_OVERRIDE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_fail);

HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_resolve);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_fail);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopAddOnContentListERKNS_7account10UserHandleE", ec_shop);
HOST_OVERRIDE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", err_show_variant);
HOST_OVERRIDE("_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", err_multimedia);

// ~DeliveryCacheFile(): the file was never opened (Open fails offline), so there is nothing to close.
OFFLINE(bcat_file_dtor, "nn::bcat::DeliveryCacheFile::~DeliveryCacheFile", 0)
HOST_OVERRIDE("_ZN2nn4bcat17DeliveryCacheFileD1Ev", bcat_file_dtor);

// void crypto::GenerateCryptographicallyRandomBytes(void *out, size_t n): the host's generator (the SDK asks csrng).
static void crypto_random(Cpu *c) {
    void *out = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (n && (!out || !plat_random(out, (size_t)n)))
        rt_fatal("nn::crypto::GenerateCryptographicallyRandomBytes: %llu bytes failed", (unsigned long long)n);
}
HOST_OVERRIDE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", crypto_random);

// void nifm::SubmitNetworkRequestAndWait(): the request ends at once, unfulfilled (IsNetworkAvailable stays false)
OFFLINE(nifm_submit_wait, "nn::nifm::SubmitNetworkRequestAndWait", 0)
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_wait);
// void ec::ShowShopApplicationInformation(ApplicationId): the eShop page is not shown
OFFLINE(ec_shop_app, "nn::ec::ShowShopApplicationInformation (eShop not available)", 0)
HOST_OVERRIDE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdE", ec_shop_app);
