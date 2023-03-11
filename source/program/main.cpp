#include "lib.hpp"
#include <stdarg.h>

namespace nn::diag::detail {
    int PrintDebugString(const char *);
}

union state_obj_callback {
    // this is actually a pointer-to-member-function
    struct {
        uint64_t _;
        uint64_t is_virt:1,
                 :63;
    };
    struct {
        uint64_t func;
        uint64_t zero;
    } nonvirt;
    struct {
        uint64_t vtable_offset;
        int64_t  is_virt:1,
                 offset_to_vtable:63;
    } virt;
};
static_assert(sizeof(union state_obj_callback) == 0x10);

struct state_obj {
    void *vt;
    void *self;
    union state_obj_callback callbacks[3];
};
static_assert(sizeof(struct state_obj) == 0x40);


struct string {
    void *vt;
    const char *str;
};

struct statemgr {
    void *vtable;
    int state;
    int counter;
    int f10;
    int f14;
    int f18;
    int f1c;
    char f21;
    char pad22[7];
    int state_objs_count;
    int pad28;
    struct state_obj *state_objs;
    int names_count;
    int pad3c;
    struct string *names;
};
static_assert(sizeof(struct statemgr) == 0x48);

static void log_str(const char *str) {
    nn::diag::detail::PrintDebugString(str);

}

__attribute__((format(printf, 1, 2)))
static void xprintf(const char *fmt, ...) {
    char buf[196];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_str(buf);
}

static const char *get_state_name(struct statemgr *smgr, int state) {
    if (state >= 0 && state < smgr->names_count)
        return smgr->names[state].str;
    else
        return "?";
}

__attribute__((noinline))
static uintptr_t get_state_callback(struct statemgr *smgr, int state, int which_callback) {
    EXL_ASSERT(state >= 0 && state < smgr->state_objs_count);
    struct state_obj *obj = &smgr->state_objs[state];
    union state_obj_callback *cb = &obj->callbacks[which_callback];
    uintptr_t func = 0;
    if (cb->is_virt) {
        void *self = obj->self;
        uintptr_t vtable = *(uintptr_t *)((char *)self + cb->virt.offset_to_vtable);
        func = *(uintptr_t *)(vtable + cb->virt.vtable_offset);
    } else {
        func = cb->nonvirt.func;
    }
    if (func)
        func -= exl::util::modules::GetTargetStart(); // unslide address
    return func;
}

static void *
return_address_from_frame_impl(void *frame0, size_t n) {
    while (n--) {
        if (!frame0)
            return nullptr;
        frame0 = ((void **)frame0)[0];
    }
    if (!frame0)
        return nullptr;
    return ((void **)frame0)[1];
}

#define return_address_from_frame(n) \
    return_address_from_frame_impl(__builtin_frame_address(0), n)

HOOK_DEFINE_TRAMPOLINE(StubStatemgrSetState) {
    static void Callback(struct statemgr *smgr, int state) {
        int old_state = smgr->state;
        xprintf("set_state(%s(%d) -> %s(%d) in:0x%lx tick:0x%lx out:0x%lx (obj:%p)) <- %p <- %p <- %p <- %p <- %p",
            get_state_name(smgr, old_state), old_state,
            get_state_name(smgr, state), state,
            get_state_callback(smgr, state, 0),
            get_state_callback(smgr, state, 1),
            get_state_callback(smgr, state, 2),
            smgr->state_objs[state].self,
            __builtin_return_address(0),
            return_address_from_frame(0),
            return_address_from_frame(1),
            return_address_from_frame(2),
            return_address_from_frame(3)
        );
        Orig(smgr, state);
    }
};

HOOK_DEFINE_TRAMPOLINE(StubOpenFile) {
    static long Callback(struct string *name, long x1, long x2, long x3, long x4, long x5) {
        char mine[256];
        const char *orig = name->str;
        if (orig) {
            size_t len = strlen(orig);
            if (len < sizeof(mine) - 1) {
                memcpy(mine, orig, len);
                mine[len] = '\0';
                while (char *s = strstr(mine, "WU")) {
                    s[0] = 'M';
                    s[1] = '1';
                }
                name->str = mine;
                long ret = Orig(name, x1, x2, x3, x4, x5);
                xprintf("open_file(%s) => %lx", name->str, ret);
                name->str = orig;
                if (ret) {
                    return ret;
                }
            }
        }
        long ret = Orig(name, x1, x2, x3, x4, x5);
        xprintf("open_file(%s) => %lx", name->str, ret);
        return ret;
    }
};

HOOK_DEFINE_TRAMPOLINE(StubSearchAssetCallTableByName) {
    static void *Callback(void *out, void *self, const char *name) {
        xprintf("searchAssetCallTableByName(%s)", name);
        return Orig(out, self, name);
    }
};

HOOK_DEFINE_TRAMPOLINE(Stub_xlink2_System_setGlobalPropertyValue) {
    static void Callback(void *self, int property_id, int value) {
        xprintf("setGlobalPropertyValue(%p, %d, %d)", self, property_id, value);
        if (property_id == 0) {
            value = 3; // NSMBU
        }
        Orig(self, property_id, value);
    }
};

HOOK_DEFINE_TRAMPOLINE(StubGetBlockInfo) {
    static int *Callback(void *block) {
        static int x = 0x100009;
        return &x;
    }
};

extern "C" void exl_main(void* x0, void* x1) {
    /* Setup hooking enviroment. */
    log_str("exl_main");
    exl::hook::Initialize();

    // this is for 3.0.1:

    //StubStatemgrSetState::InstallAtOffset(0x8b9280);
    //StubOpenFile::InstallAtOffset(0x008b7b80);
    //StubWtf::InstallAtOffset(0x1bc1590);
    StubSearchAssetCallTableByName::InstallAtOffset(0x005ac9e0);
    Stub_xlink2_System_setGlobalPropertyValue::InstallAtOffset(0x5a3490);
    StubGetBlockInfo::InstallAtOffset(0x00e25ae0);

    {
        // Patch to skip intro cutscene
        exl::patch::CodePatcher p(0x017e428c);
        p.Write<uint32_t>(0x321e03e1); // orr w1, wzr, #4 (instead of 2)
    }

    log_str("done hooking");
}

extern "C" NORETURN void exl_exception_entry() {
    /* TODO: exception handling */
    EXL_ABORT(0x420);
}
