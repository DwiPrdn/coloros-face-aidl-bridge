#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#define LOG_TAG "FaceAidlBridge"
#include "bridge_log.h"

// Platform LL-NDK libbinder_ndk functions
extern "C" {
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_startThreadPool(void);
void ABinderProcess_joinThreadPool(void);
void AIBinder_markVintfStability(AIBinder* binder);
}

// libmiface.so entrypoints (same as /vendor/bin/hw/mifaced)
namespace miface {
namespace service {
void register_face_service();
void register_aon_service();
}  // namespace service
}  // namespace miface

namespace {

static constexpr const char* kIFaceDescriptor = "android.hardware.biometrics.face.IFace";
static constexpr const char* kIFaceInstance = "android.hardware.biometrics.face.IFace/default";

// IFace transactions
static constexpr transaction_code_t kTxCreateSession = 2;

// Face AIDL V1 ISessionCallback transactions (verified from android.hardware.biometrics.face-V1-ndk_platform.so)
static constexpr transaction_code_t kCbTxOnEnrollmentFrame = 4;
static constexpr transaction_code_t kCbTxOnError = 5;
static constexpr transaction_code_t kCbTxOnEnrollmentProgress = 6;
static constexpr transaction_code_t kCbTxOnAuthSucceeded = 7;
static constexpr transaction_code_t kCbTxOnAuthFailed = 8;
static constexpr transaction_code_t kCbTxOnSessionClosed = 19;

// Oplus custom IFace binder transactions (from ColorOS 17 FaceProviderExtImpl)
static constexpr transaction_code_t kOplusTxUpdateSettingSwitch = 1001;  // 0x3e9
static constexpr transaction_code_t kOplusTxSendFaceCmd = 1002;          // 0x3ea
static constexpr transaction_code_t kOplusTxUpdateDynLog = 1005;         // 0x3ed

// Zero-collision callback tracking:
// We NEVER define a second AIBinder_Class for ISessionCallback (which collided with
// android.hardware.biometrics.face-V1-ndk_platform.so's class inside libmiface.so!).
// CRITICAL: We hold a strong ref to origCb across forwardRawTransaction (and until
// onSessionClosed / next createSession) so libbinder_ndk's ABpBinder cache returns the
// EXACT SAME AIBinder* pointer when libmiface.so deserializes the callback!
static std::mutex gFaceCbMutex;
static std::atomic<AIBinder*> gActiveFaceCb{nullptr};
static std::atomic<int32_t> gLastProgressRem{5};
static std::atomic<uint32_t> gEnrollFrameLogCount{0};

static void setActiveFaceCb(AIBinder* newCb) {
    std::lock_guard<std::mutex> lk(gFaceCbMutex);
    AIBinder* oldCb = gActiveFaceCb.exchange(newCb, std::memory_order_acq_rel);
    if (oldCb) {
        AIBinder_decStrong(oldCb);
    }
}

static void clearActiveFaceCbIfMatch(AIBinder* cb) {
    std::lock_guard<std::mutex> lk(gFaceCbMutex);
    AIBinder* cur = gActiveFaceCb.load(std::memory_order_acquire);
    if (cur && cur == cb) {
        gActiveFaceCb.store(nullptr, std::memory_order_release);
        AIBinder_decStrong(cur);
    }
}

// AIDL face::AcquiredInfo::VENDOR == 23 (maps to Java FaceManager FACE_ACQUIRED_VENDOR == 22).
// Xiaomi libmiface emits internal status codes 7587 (camera opening) and 1001 (session active)
// as VENDOR frames; ColorOS ScreenLockFaceEnrollController does not recognize them and counts
// each as a failed attempt toward its 5-strike auto-cancel limit!
static constexpr int8_t kAidlFaceAcquiredVendor = 23;
static constexpr int32_t kBaseFrameParcelSize = 28;

using AddServiceFn = binder_status_t (*)(AIBinder*, const char*);
using TransactFn = binder_status_t (*)(AIBinder*, transaction_code_t, AParcel**, AParcel**, binder_flags_t);
using GetPlaneDataFn = int32_t (*)(void*, int, uint8_t**, int*);

static AddServiceFn getRealAddService() {
    static AddServiceFn realFn = nullptr;
    if (!realFn) {
        realFn = reinterpret_cast<AddServiceFn>(dlsym(RTLD_NEXT, "AServiceManager_addService"));
        if (!realFn) {
            void* handle = dlopen("libbinder_ndk.so", RTLD_LAZY | RTLD_NOLOAD);
            if (!handle) handle = dlopen("libbinder_ndk.so", RTLD_LAZY);
            if (handle) {
                realFn = reinterpret_cast<AddServiceFn>(dlsym(handle, "AServiceManager_addService"));
            }
        }
    }
    return realFn;
}

static TransactFn getRealTransact() {
    static TransactFn realFn = nullptr;
    if (!realFn) {
        realFn = reinterpret_cast<TransactFn>(dlsym(RTLD_NEXT, "AIBinder_transact"));
        if (!realFn) {
            void* handle = dlopen("libbinder_ndk.so", RTLD_LAZY | RTLD_NOLOAD);
            if (!handle) handle = dlopen("libbinder_ndk.so", RTLD_LAZY);
            if (handle) {
                realFn = reinterpret_cast<TransactFn>(dlsym(handle, "AIBinder_transact"));
            }
        }
    }
    return realFn;
}

static GetPlaneDataFn getRealGetPlaneData() {
    static GetPlaneDataFn realFn = nullptr;
    if (!realFn) {
        realFn = reinterpret_cast<GetPlaneDataFn>(dlsym(RTLD_NEXT, "AImage_getPlaneData"));
        if (!realFn) {
            void* handle = dlopen("libmediandk.so", RTLD_LAZY | RTLD_NOLOAD);
            if (!handle) handle = dlopen("libmediandk.so", RTLD_LAZY);
            if (handle) {
                realFn = reinterpret_cast<GetPlaneDataFn>(dlsym(handle, "AImage_getPlaneData"));
            }
        }
    }
    return realFn;
}

static void writeOkStatus(AParcel* out) {
    AStatus* st = AStatus_newOk();
    AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
}

static bool readByteArrayHelper(const AParcel* in, std::vector<uint8_t>& out) {
    auto alloc = [](void* vectorData, int32_t length, int8_t** outBuffer) -> bool {
        auto* vec = static_cast<std::vector<uint8_t>*>(vectorData);
        if (length < 0) {
            vec->clear();
            *outBuffer = nullptr;
            return true;
        }
        vec->resize(static_cast<size_t>(length));
        *outBuffer = reinterpret_cast<int8_t*>(vec->data());
        return true;
    };
    return AParcel_readByteArray(in, &out, alloc) == STATUS_OK;
}

static binder_status_t forwardRawTransaction(
        AIBinder* target, transaction_code_t code, const AParcel* in, AParcel* out) {
    TransactFn realTransact = getRealTransact();
    if (!realTransact) return STATUS_UNKNOWN_ERROR;

    AParcel* fwdIn = nullptr;
    binder_status_t st = AIBinder_prepareTransaction(target, &fwdIn);
    if (st != STATUS_OK) {
        ALOGE("forwardRaw(tx=%u): prepareTransaction failed: %d", code, st);
        return st;
    }

    int32_t inPos = AParcel_getDataPosition(in);
    int32_t inSize = AParcel_getDataSize(in);
    if (inSize > inPos) {
        st = AParcel_appendFrom(in, fwdIn, inPos, inSize - inPos);
        if (st != STATUS_OK) {
            ALOGE("forwardRaw(tx=%u): appendFrom(in) failed: %d", code, st);
            AParcel_delete(fwdIn);
            return st;
        }
    }

    AParcel* fwdOut = nullptr;
    st = realTransact(target, code, &fwdIn, &fwdOut, 0);
    if (st != STATUS_OK) {
        ALOGE("forwardRaw(tx=%u): transact failed: %d", code, st);
        if (fwdOut) AParcel_delete(fwdOut);
        return st;
    }

    if (fwdOut) {
        int32_t outSize = AParcel_getDataSize(fwdOut);
        if (outSize > 0) {
            st = AParcel_appendFrom(fwdOut, out, 0, outSize);
            if (st != STATUS_OK) {
                ALOGE("forwardRaw(tx=%u): appendFrom(out) failed: %d", code, st);
            }
        }
        AParcel_delete(fwdOut);
    }
    return st;
}

static void sendSyntheticEnrollmentProgress(
        AIBinder* cb, int32_t enrollId, int32_t rem, binder_flags_t flags) {
    TransactFn realTransact = getRealTransact();
    if (!realTransact || !cb) return;
    AParcel* synIn = nullptr;
    AParcel* synOut = nullptr;
    binder_status_t st = AIBinder_prepareTransaction(cb, &synIn);
    if (st != STATUS_OK) {
        ALOGW("sendSyntheticEnrollmentProgress(%d,%d): prepare failed %d", enrollId, rem, st);
        return;
    }
    AParcel_writeInt32(synIn, enrollId);
    AParcel_writeInt32(synIn, rem);
    st = realTransact(cb, kCbTxOnEnrollmentProgress, &synIn, &synOut, flags);
    if (st != STATUS_OK) {
        ALOGW("sendSyntheticEnrollmentProgress(%d,%d): transact failed %d", enrollId, rem, st);
    }
    if (synOut) {
        AParcel_delete(synOut);
    }
}

// ============================================================================
// IFace Proxy (ColorOS 17 Custom Transactions + Transparent createSession)
// ============================================================================

struct FaceProxyData {
    AIBinder* target;  // Strong reference to libmiface's real BnFace AIBinder
};

static void* onCreateProxy(void* args) { return args; }
static void onDestroyProxy(void* userData) {
    auto* data = static_cast<FaceProxyData*>(userData);
    if (data) {
        if (data->target) {
            AIBinder_decStrong(data->target);
        }
        delete data;
    }
}

static binder_status_t onTransactProxy(
        AIBinder* binder, transaction_code_t code, const AParcel* in, AParcel* out) {
    auto* data = static_cast<FaceProxyData*>(AIBinder_getUserData(binder));
    if (!data || !data->target) {
        return STATUS_BAD_VALUE;
    }

    // Intercept ColorOS custom transactions on IFace/default
    if (code == kOplusTxUpdateSettingSwitch) {
        int32_t switchType = 0;
        bool on = false;
        AParcel_readInt32(in, &switchType);
        AParcel_readBool(in, &on);
        ALOGI("Oplus updateSettingSwitchStatus(1001): switchType=%d on=%d -> OK(0)",
              switchType, on ? 1 : 0);
        writeOkStatus(out);
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }

    if (code == kOplusTxSendFaceCmd) {
        int32_t cmdId = 0;
        int32_t bufLen = 0;
        std::vector<uint8_t> buf;
        AParcel_readInt32(in, &cmdId);
        AParcel_readInt32(in, &bufLen);
        readByteArrayHelper(in, buf);
        ALOGI("Oplus sendFaceCmd(1002): cmdId=%d bufLen=%d actualSize=%zu -> OK(0)",
              cmdId, bufLen, buf.size());
        writeOkStatus(out);
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }

    if (code == kOplusTxUpdateDynLog) {
        int32_t arg1 = 0, arg2 = 0;
        AParcel_readInt32(in, &arg1);
        AParcel_readInt32(in, &arg2);
        ALOGI("Oplus updateDynamicallyLog(1005): arg1=%d arg2=%d -> OK", arg1, arg2);
        writeOkStatus(out);
        return STATUS_OK;
    }

    if (code >= 1000 && code < 0x00ffff00u) {
        ALOGI("Oplus unknown custom IFace tx=%u -> OK(0)", code);
        writeOkStatus(out);
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }

    // Peek IFace::createSession(int32 sensorId, int32 userId, ISessionCallback cb)
    // without modifying or wrapping cb (prevents AIBinder_Class descriptor collision!).
    // Keep origCb's strong ref pinned in gActiveFaceCb across forwardRawTransaction so
    // libbinder_ndk returns the EXACT SAME AIBinder* pointer when libmiface reads the parcel.
    if (code == kTxCreateSession) {
        int32_t savedPos = AParcel_getDataPosition(in);
        int32_t sensorId = 0, userId = 0;
        AIBinder* origCb = nullptr;
        AParcel_readInt32(in, &sensorId);
        AParcel_readInt32(in, &userId);
        AParcel_readStrongBinder(in, &origCb);
        AParcel_setDataPosition(in, savedPos);

        setActiveFaceCb(origCb);  // takes ownership of origCb's strong ref from readStrongBinder
        gLastProgressRem.store(5, std::memory_order_release);
        gEnrollFrameLogCount.store(0, std::memory_order_release);
        ALOGI("IFace::createSession(sensorId=%d, userId=%d, origCb=%p) -> transparent pass-through (pinned)",
              sensorId, userId, origCb);
        return forwardRawTransaction(data->target, code, in, out);
    }

    // Forward all other standard AIDL IFace transactions directly to libmiface's BnFace
    return forwardRawTransaction(data->target, code, in, out);
}

static AIBinder_Class* gFaceProxyClass = nullptr;

static AIBinder* wrapFaceBinder(AIBinder* rawFaceBinder) {
    if (!gFaceProxyClass) {
        gFaceProxyClass = AIBinder_Class_define(
                kIFaceDescriptor, onCreateProxy, onDestroyProxy, onTransactProxy);
    }
    AIBinder_incStrong(rawFaceBinder);
    auto* data = new FaceProxyData{rawFaceBinder};
    AIBinder* proxy = AIBinder_new(gFaceProxyClass, data);
    AIBinder_markVintfStability(proxy);
    return proxy;
}

static std::atomic<uint32_t> gLumaFrameCount{0};
static std::atomic<int64_t> gLastLumaNs{0};

}  // namespace

// Interpose AServiceManager_addService (version LIBBINDER_NDK)
extern "C" __attribute__((visibility("default")))
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance) {
    AddServiceFn realAddService = getRealAddService();
    if (!realAddService) {
        ALOGE("Failed to resolve real AServiceManager_addService!");
        return STATUS_UNKNOWN_ERROR;
    }

    if (instance && strcmp(instance, kIFaceInstance) == 0 && binder != nullptr) {
        ALOGI("Intercepting %s registration -> wrapping libmiface BnFace (uid=%u gid=%u)",
              instance, getuid(), getgid());
        AIBinder* wrapped = wrapFaceBinder(binder);
        binder_status_t ret = realAddService(wrapped, instance);
        ALOGI("Registered wrapped %s: status=%d", instance, ret);
        return ret;
    }

    ALOGI("Passing through AServiceManager_addService(%s)", instance ? instance : "(null)");
    return realAddService(binder, instance);
}

// Interpose AIBinder_transact (version LIBBINDER_NDK) to observe & enhance
// libmiface's outbound ISessionCallback calls with ZERO AIBinder_Class collision.
extern "C" __attribute__((visibility("default")))
binder_status_t AIBinder_transact(
        AIBinder* binder, transaction_code_t code, AParcel** in, AParcel** out,
        binder_flags_t flags) {
    TransactFn realTransact = getRealTransact();
    if (!realTransact) {
        return STATUS_UNKNOWN_ERROR;
    }

    static thread_local bool tInHook = false;
    bool clearCbAfterTransact = false;
    if (!tInHook && binder != nullptr && binder == gActiveFaceCb.load(std::memory_order_acquire)) {
        tInHook = true;
        if (code == kCbTxOnEnrollmentProgress && in && *in) {
            // Parcel ends with (int32 enrollmentId, int32 remaining)
            int32_t origPos = AParcel_getDataPosition(*in);
            int32_t sz = AParcel_getDataSize(*in);
            if (sz >= 8) {
                int32_t enrollId = 0, remaining = -1;
                AParcel_setDataPosition(*in, sz - 8);
                AParcel_readInt32(*in, &enrollId);
                AParcel_readInt32(*in, &remaining);
                AParcel_setDataPosition(*in, origPos);
                int32_t prev = gLastProgressRem.load(std::memory_order_acquire);
                ALOGI("ISessionCallback::onEnrollmentProgress(id=%d, remaining=%d, prev=%d)",
                      enrollId, remaining, prev);
                if (remaining > 0) {
                    // ColorOS FaceEnrollNewActivity ONLY starts the circular progress morph &
                    // Lottie ring animation when remaining == 4. Map arbitrary multi-step counts
                    // monotonically onto 4 -> 3 -> 2 -> 1.
                    int32_t mapped = (prev == 5) ? 4 : ((prev > 1) ? (prev - 1) : 1);
                    if (mapped != remaining) {
                        AParcel_setDataPosition(*in, sz - 4);
                        AParcel_writeInt32(*in, mapped);
                        AParcel_setDataPosition(*in, origPos);
                        ALOGI("Mapped intermediate face progress %d -> %d (id=%d)",
                              remaining, mapped, enrollId);
                    }
                    gLastProgressRem.store(mapped, std::memory_order_release);
                } else if (remaining == 0) {
                    if (prev == 5) {
                        // One-shot enrollment: emit 4, wait 500ms for FaceEnrollNewActivity's
                        // 250ms startDelay + 200ms fade-in of mEnrollingProgressCircle, then
                        // step 3 -> 2 -> 1 -> real 0 at 200ms cadence.
                        ALOGI("Synthesizing ColorOS 5-step face progress (4->3->2->1->0) for id=%d",
                              enrollId);
                        sendSyntheticEnrollmentProgress(binder, enrollId, 4, flags);
                        usleep(500000);
                        for (int32_t step = 3; step >= 1; --step) {
                            sendSyntheticEnrollmentProgress(binder, enrollId, step, flags);
                            usleep(200000);
                        }
                    } else if (prev > 1) {
                        ALOGI("Synthesizing remaining ColorOS face steps (%d->1->0) for id=%d",
                              prev - 1, enrollId);
                        if (prev == 4) {
                            usleep(350000);
                        }
                        for (int32_t step = prev - 1; step >= 1; --step) {
                            sendSyntheticEnrollmentProgress(binder, enrollId, step, flags);
                            usleep(200000);
                        }
                    }
                    gLastProgressRem.store(5, std::memory_order_release);
                }
            }
        } else if (code == kCbTxOnError) {
            ALOGI("ISessionCallback::onError()");
            gLastProgressRem.store(5, std::memory_order_release);
        } else if (code == kCbTxOnAuthSucceeded) {
            ALOGI("ISessionCallback::onAuthenticationSucceeded()");
        } else if (code == kCbTxOnAuthFailed) {
            ALOGI("ISessionCallback::onAuthenticationFailed()");
        } else if (code == kCbTxOnSessionClosed) {
            ALOGI("ISessionCallback::onSessionClosed()");
            gLastProgressRem.store(5, std::memory_order_release);
            clearCbAfterTransact = true;
        } else if (code == kCbTxOnEnrollmentFrame && in && *in) {
            // EnrollmentFrame ends with fixed-size 28-byte BaseFrame:
            //   [sz-28]: int32 parcelSize (28)
            //   [sz-24]: byte acquiredInfo (4 bytes)
            //   [sz-20]: int32 vendorCode  (4 bytes)
            //   [sz-16..sz-4]: float pan, float tilt, float distance, bool isCancellable
            int32_t origPos = AParcel_getDataPosition(*in);
            int32_t sz = AParcel_getDataSize(*in);
            if (sz >= kBaseFrameParcelSize) {
                int32_t baseSize = 0, vendorCode = 0;
                int8_t acqInfo = 0;
                AParcel_setDataPosition(*in, sz - kBaseFrameParcelSize);
                AParcel_readInt32(*in, &baseSize);
                AParcel_readByte(*in, &acqInfo);
                AParcel_readInt32(*in, &vendorCode);
                AParcel_setDataPosition(*in, origPos);
                if (baseSize == kBaseFrameParcelSize) {
                    uint32_t n = gEnrollFrameLogCount.fetch_add(1) + 1;
                    if (acqInfo == kAidlFaceAcquiredVendor &&
                        (vendorCode == 7587 || vendorCode == 1001)) {
                        ALOGI("ISessionCallback::onEnrollmentFrame(acq=%d, vendor=%d) -> suppressed Xiaomi internal status",
                              static_cast<int>(acqInfo), vendorCode);
                        AParcel_delete(*in);
                        *in = nullptr;
                        if (out) {
                            if ((flags & FLAG_ONEWAY) == 0) {
                                // BpSessionCallback::onEnrollmentFrame is synchronous and always
                                // calls AParcel_readStatusHeader(*out, &status) on STATUS_OK!
                                AParcel* okOut = AParcel_create();
                                if (okOut) {
                                    writeOkStatus(okOut);
                                    AParcel_setDataPosition(okOut, 0);
                                }
                                *out = okOut;
                            } else {
                                *out = nullptr;
                            }
                        }
                        tInHook = false;
                        return STATUS_OK;
                    }
                    if (n <= 10 || (n % 30) == 0) {
                        ALOGI("ISessionCallback::onEnrollmentFrame#%u(acq=%d, vendor=%d)",
                              n, static_cast<int>(acqInfo), vendorCode);
                    }
                }
            }
        }
        tInHook = false;
    }

    binder_status_t ret = realTransact(binder, code, in, out, flags);
    if (clearCbAfterTransact) {
        clearActiveFaceCbIfMatch(binder);
    }
    return ret;
}

// Interpose AImage_getPlaneData (version LIBMEDIANDK) to sample Y-plane brightness
// on frames 1, 5, 15, 30, 60 for diagnostics (negligible overhead).
extern "C" __attribute__((visibility("default")))
int32_t AImage_getPlaneData(void* image, int planeIdx, uint8_t** data, int* dataLength) {
    GetPlaneDataFn realFn = getRealGetPlaneData();
    if (!realFn) return -10000;
    int32_t ret = realFn(image, planeIdx, data, dataLength);
    if (BRIDGE_DEBUG_ENABLED() && ret == 0 && planeIdx == 0 && data && *data && dataLength && *dataLength > 0) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t nowNs = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        int64_t prevNs = gLastLumaNs.exchange(nowNs);
        if (nowNs - prevNs > 2000000000LL) {
            gLumaFrameCount.store(0);
        }
        uint32_t cnt = gLumaFrameCount.fetch_add(1) + 1;
        if (cnt == 1 || cnt == 5 || cnt == 15 || cnt == 30 || cnt == 60) {
            const uint8_t* p = *data;
            int len = *dataLength;
            uint32_t minV = 255, maxV = 0;
            uint64_t sum = 0, samples = 0;
            for (int i = 0; i < len; i += 64) {
                uint8_t v = p[i];
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
                sum += v;
                samples++;
            }
            ALOGI("Camera Y-plane sample frame#%u: len=%d min=%u max=%u mean=%llu",
                  cnt, len, minV, maxV, samples ? (unsigned long long)(sum / samples) : 0ULL);
        }
    }
    return ret;
}

static int runSelfTest() {
    printf("[FaceAidlBridge v6] Running self-test...\n");
    static AIBinder* sCreateSessionCb = nullptr;
    sCreateSessionCb = nullptr;
    auto dummyCreate = [](void* a) -> void* { return a; };
    auto dummyDestroy = [](void*) {};
    auto dummyTransact = [](AIBinder*, transaction_code_t code, const AParcel* in, AParcel* out) -> binder_status_t {
        if (code == kTxCreateSession) {
            int32_t sid = 0, uid = 0;
            AIBinder* cb = nullptr;
            AParcel_readInt32(in, &sid);
            AParcel_readInt32(in, &uid);
            AParcel_readStrongBinder(in, &cb);
            sCreateSessionCb = cb;
            if (cb) AIBinder_decStrong(cb);
            writeOkStatus(out);
            AParcel_writeStrongBinder(out, nullptr);
            return STATUS_OK;
        }
        writeOkStatus(out);
        if (code == 16777215) {
            AParcel_writeInt32(out, 1);
        }
        return STATUS_OK;
    };
    AIBinder_Class* cls = AIBinder_Class_define(kIFaceDescriptor, dummyCreate, dummyDestroy, dummyTransact);
    AIBinder* target = AIBinder_new(cls, nullptr);
    AIBinder* wrapped = wrapFaceBinder(target);

    // 1. Test forwarded getInterfaceVersion (16777215)
    AParcel* in = nullptr;
    AParcel* out = nullptr;
    AIBinder_prepareTransaction(wrapped, &in);
    binder_status_t st = AIBinder_transact(wrapped, 16777215, &in, &out, 0);
    AStatus* rstat = nullptr;
    int32_t ver = -1;
    if (st == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &rstat);
        AParcel_readInt32(out, &ver);
        AParcel_delete(out);
    }
    printf("  forwarded getInterfaceVersion: st=%d ok=%d ver=%d\n",
           st, rstat ? AStatus_isOk(rstat) : 0, ver);
    if (rstat) AStatus_delete(rstat);

    // 2. Test Oplus sendFaceCmd (1002)
    in = nullptr; out = nullptr; rstat = nullptr;
    AIBinder_prepareTransaction(wrapped, &in);
    AParcel_writeInt32(in, 42);
    AParcel_writeInt32(in, 2);
    int8_t payload[2] = {1, 2};
    AParcel_writeByteArray(in, payload, 2);
    st = AIBinder_transact(wrapped, kOplusTxSendFaceCmd, &in, &out, 0);
    int32_t cmdRet = -1;
    if (st == STATUS_OK && out) {
        AParcel_readStatusHeader(out, &rstat);
        AParcel_readInt32(out, &cmdRet);
        AParcel_delete(out);
    }
    printf("  Oplus sendFaceCmd(1002): st=%d ok=%d ret=%d\n",
           st, rstat ? AStatus_isOk(rstat) : 0, cmdRet);
    if (rstat) AStatus_delete(rstat);

    // 3. Test IFace::createSession callback pinning + one-shot 4->3->2->1->0 synthesis
    //    + intermediate remapping (99 -> 4, then 0 -> 3,2,1,0) + VENDOR 7587/1001 suppression
    static std::vector<int32_t> sReceivedRem;
    static int32_t sEnrollFramesForwarded = 0;
    sReceivedRem.clear();
    sEnrollFramesForwarded = 0;
    auto cbTransact = [](AIBinder*, transaction_code_t code, const AParcel* cbIn, AParcel* cbOut) -> binder_status_t {
        if (code == kCbTxOnEnrollmentProgress) {
            int32_t id = 0, rem = -1;
            AParcel_readInt32(cbIn, &id);
            AParcel_readInt32(cbIn, &rem);
            sReceivedRem.push_back(rem);
        } else if (code == kCbTxOnEnrollmentFrame) {
            sEnrollFramesForwarded++;
        }
        writeOkStatus(cbOut);
        return STATUS_OK;
    };
    AIBinder_Class* cbCls = AIBinder_Class_define("test.mock.FaceCb", dummyCreate, dummyDestroy, cbTransact);
    AIBinder* mockCb = AIBinder_new(cbCls, nullptr);

    // 3a. Call IFace::createSession through wrapped proxy to verify cb pinning
    in = nullptr; out = nullptr;
    AIBinder_prepareTransaction(wrapped, &in);
    AParcel_writeInt32(in, 0);
    AParcel_writeInt32(in, 0);
    AParcel_writeStrongBinder(in, mockCb);
    st = AIBinder_transact(wrapped, kTxCreateSession, &in, &out, 0);
    if (out) AParcel_delete(out);
    bool pinOk = (sCreateSessionCb == mockCb && gActiveFaceCb.load() == mockCb);
    printf("  createSession cb pinning: %s (mock=%p active=%p fwd=%p)\n",
           pinOk ? "OK" : "FAIL", mockCb, gActiveFaceCb.load(), sCreateSessionCb);

    // 3b. Test one-shot onEnrollmentProgress(0) -> [4,3,2,1,0]
    in = nullptr; out = nullptr;
    AIBinder_prepareTransaction(mockCb, &in);
    AParcel_writeInt32(in, 1);
    AParcel_writeInt32(in, 0);
    st = AIBinder_transact(mockCb, kCbTxOnEnrollmentProgress, &in, &out, 0);
    if (out) AParcel_delete(out);
    std::vector<int32_t> oneShotSteps = sReceivedRem;

    // 3c. Test intermediate remapping: 99 -> 4 (in-place overwrite), then 0 -> 3,2,1,0
    sReceivedRem.clear();
    in = nullptr; out = nullptr;
    AIBinder_prepareTransaction(mockCb, &in);
    AParcel_writeInt32(in, 2);
    AParcel_writeInt32(in, 99);
    AIBinder_transact(mockCb, kCbTxOnEnrollmentProgress, &in, &out, 0);
    if (out) AParcel_delete(out);
    in = nullptr; out = nullptr;
    AIBinder_prepareTransaction(mockCb, &in);
    AParcel_writeInt32(in, 2);
    AParcel_writeInt32(in, 0);
    AIBinder_transact(mockCb, kCbTxOnEnrollmentProgress, &in, &out, 0);
    if (out) AParcel_delete(out);
    std::vector<int32_t> mappedSteps = sReceivedRem;

    // 3d. Test VENDOR 7587/1001 suppression (including synchronous AParcel_readStatusHeader(*out))
    //     vs normal frame pass-through
    bool suppressReplyOk = true;
    auto sendMockEnrollFrame = [&](int8_t acq, int32_t vendor) {
        AParcel* fIn = nullptr;
        AParcel* fOut = nullptr;
        AIBinder_prepareTransaction(mockCb, &fIn);
        // Trailing 28-byte BaseFrame
        AParcel_writeInt32(fIn, kBaseFrameParcelSize);
        AParcel_writeByte(fIn, acq);
        AParcel_writeInt32(fIn, vendor);
        AParcel_writeFloat(fIn, 0.0f);
        AParcel_writeFloat(fIn, 0.0f);
        AParcel_writeFloat(fIn, 0.0f);
        AParcel_writeBool(fIn, false);
        binder_status_t fst = AIBinder_transact(mockCb, kCbTxOnEnrollmentFrame, &fIn, &fOut, 0);
        if (fst != STATUS_OK || fOut == nullptr) {
            suppressReplyOk = false;
        } else {
            AStatus* fStatus = nullptr;
            binder_status_t rst = AParcel_readStatusHeader(fOut, &fStatus);
            if (rst != STATUS_OK || !fStatus || !AStatus_isOk(fStatus)) {
                suppressReplyOk = false;
            }
            if (fStatus) AStatus_delete(fStatus);
            AParcel_delete(fOut);
        }
    };
    sendMockEnrollFrame(kAidlFaceAcquiredVendor, 7587);  // should be suppressed with valid OK reply
    sendMockEnrollFrame(kAidlFaceAcquiredVendor, 1001);  // should be suppressed with valid OK reply
    sendMockEnrollFrame(12, 5);                          // NOT_DETECTED -> should pass through

    // 3e. Test onSessionClosed clears gActiveFaceCb
    in = nullptr; out = nullptr;
    AIBinder_prepareTransaction(mockCb, &in);
    AIBinder_transact(mockCb, kCbTxOnSessionClosed, &in, &out, 0);
    if (out) AParcel_delete(out);
    bool closedOk = (gActiveFaceCb.load() == nullptr);

    printf("  one-shot steps=%zu [", oneShotSteps.size());
    for (size_t i = 0; i < oneShotSteps.size(); ++i) printf("%s%d", i ? "," : "", oneShotSteps[i]);
    printf("], remapped steps=%zu [", mappedSteps.size());
    for (size_t i = 0; i < mappedSteps.size(); ++i) printf("%s%d", i ? "," : "", mappedSteps[i]);
    printf("], enrollFramesFwd=%d suppressReplyOk=%d closedOk=%d\n",
           sEnrollFramesForwarded, suppressReplyOk ? 1 : 0, closedOk ? 1 : 0);
    AIBinder_decStrong(mockCb);

    printf("  real addService=%p transact=%p getPlaneData=%p\n",
           reinterpret_cast<void*>(getRealAddService()),
           reinterpret_cast<void*>(getRealTransact()),
           reinterpret_cast<void*>(getRealGetPlaneData()));

    AIBinder_decStrong(wrapped);
    AIBinder_decStrong(target);
    auto is5Steps = [](const std::vector<int32_t>& v) {
        return v.size() == 5 && v[0] == 4 && v[1] == 3 && v[2] == 2 && v[3] == 1 && v[4] == 0;
    };
    return (ver == 1 && cmdRet == 0 && pinOk && is5Steps(oneShotSteps) && is5Steps(mappedSteps) &&
            sEnrollFramesForwarded == 1 && suppressReplyOk && closedOk &&
            getRealAddService() != nullptr && getRealTransact() != nullptr)
                   ? 0
                   : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        return runSelfTest();
    }

    BRIDGE_LOG_BOOT("FaceAidlBridge v6 starting (uid=%u gid=%u, debug prop=%s)...",
                    getuid(), getgid(), cos_bridge_log::kDebugPropName);
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    miface::service::register_face_service();
    ALOGI("register_face_service() completed");

    miface::service::register_aon_service();
    BRIDGE_LOG_BOOT("Registered Face + AON services successfully!");
    ABinderProcess_joinThreadPool();
    ALOGE("ABinderProcess_joinThreadPool() exited unexpectedly!");
    return 1;
}
