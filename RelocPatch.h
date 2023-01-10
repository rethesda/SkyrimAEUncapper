/**
 * @file RelocPatch.h
 * @author Andrew Spaulding (Kasplat)
 * @brief Provides an interface for finding assembly hooks within the AE EXE.
 * @bug No known bugs.
 *
 * This file is intended to replace the previous implementation of address
 * finding, which required modifying the RelocAddr template in the SKSE source
 * so that the overload of the assignment operator could be made public. This
 * template instead takes in a signature which it will resolve either when
 * asked to do so or when doing so is necessary for the current opperator.
 *
 * The template has intentionally been written so that it can be constructed in
 * constant time and resolved in non-constant time, to avoid any wierdness with
 * C++ and its lack of "delayed init" idioms.
 *
 * I miss Rust.
 */

#ifndef __SKYRIM_UNCAPPER_AE_RELOC_PATCH_H__
#define __SKYRIM_UNCAPPER_AE_RELOC_PATCH_H__

#include <cstddef>
#include <cstdint>

#include "common/IErrors.h"
#include "Relocation.h"
#include "SafeWrite.h"
#include "reg2k/RVA.h"

#include "Signatures.h"
#include "SafeMemSet.h"

/// @brief The maximum size of an instruction in the x86 ISA.
const size_t kMaxInstrSize = 15;

/// @brief The opcode for an x86 NOP.
const uint8_t kNop = 0x90;

/// @brief Patch sizes for direct hooks.
///@{
const size_t kDirectCallPatchSize = 5;
const size_t kDirectJumpPatchSize = 5;
///@}

/// @brief Encodes the various types of hooks which can be injected.
class HookType {
public:
    enum t { None, Branch5, Branch6, Call5, Call6, DirectCall, DirectJump, Nop };

    static size_t
    Size(
        t type
    ) {
        size_t ret = 0;

        switch (type) {
            case Nop:
                ret = 0;
                break;
            case Branch5:
            case Call5:
                ret = 5;
                break;
            case Branch6:
            case Call6:
                ret = 6;
                break;
            case DirectCall:
                ret = kDirectCallPatchSize;
                break;
            case DirectJump:
                ret = kDirectJumpPatchSize;
                break;
            default:
                HALT("Cannot get the size of an invalid hook type.");
        }

        return ret;
    }
};

/// @brief Describes a patch to be applied by a RelocPatch<T>.
struct PatchSignature {
    const char* name;
    HookType::t hook_type;
    const char* sig;
    size_t patch_size;
    ptrdiff_t hook_offset;
    ptrdiff_t indirect_offset;
    size_t instr_size;

    /**
     * @brief Creates a new patch signature structure.
     * @param name The name of the patch signature.
     * @param hook_type The type of hook to be inserted, or None.
     * @param sig The hex signature to search for.
     * @param patch_size The size of the code to be overwritten.
     * @param offset The offset from the signature to the hook address.
     * @param indirect_offset An indirection offset to use to dereference the
     *                        first hook address to get a second, or 0.
     * @param instr_size The size of the instruction being indirected through,
     *                   or 0.
     */
    PatchSignature(
        const char* name,
        HookType::t hook_type,
        const char* sig,
        size_t patch_size,
        size_t hook_offset = 0,
        size_t indirect_offset = 0,
        size_t instr_size = 0
    ) : name(name),
        hook_type(hook_type),
        sig(sig),
        patch_size(patch_size),
        hook_offset(hook_offset),
        indirect_offset(indirect_offset),
        instr_size(instr_size)
    {}
};

template<typename T>
class RelocPatch {
  private:
    const PatchSignature *sig;
    bool hook_done = false;
    uintptr_t real_address = 0;

  public:
    /**
     * @brief Constructs a new relocatable patch.
     * @param sig The signature describing the patch.
     */
    RelocPatch(
        const PatchSignature *sig
    ) : sig(sig)
    {}

    /**
     * @brief Dereferences the data contained by the patch
     */
    T &operator *() {
        Resolve();
        return * reinterpret_cast<T*>(real_address);
    }

    /**
     * @brief Dereferences this relocatable patch to its underlying data.
     */
    T *operator->() {
        Resolve();
        return reinterpret_cast<T*>(real_address);
    }

    /**
     * @brief Allows functions contained as T to be called directly through this class.
     */
    operator T() {
        return reinterpret_cast<T>(real_address);
    }

    /**
     * @brief Resolves the underlying address, if it has not already done so.
     */
    void
    Resolve() {
        if (real_address) { return; }

        auto rva = RVAScan<T>(
            sig->name,
            sig->sig,
            sig->hook_offset,
            sig->indirect_offset,
            sig->instr_size
        );
        real_address = rva.GetUIntPtr();

        ASSERT(real_address);
    }

    /**
     * @brief Gets the effective address of the found hook.
     */
    uintptr_t
    GetUIntPtr() {
        Resolve();
        return real_address;
    }

    /**
     * @brief Gets the address that should be returned to from the patch.
     */
    uintptr_t
    GetRetAddr() {
        Resolve();
        return real_address + HookType::Size(sig->hook_type);
    }

    /**
     * @brief Writes the patch, redirecting to the given address if applicable.
     *
     * It is illegal to apply a patch more than once.
     */
    void
    Apply(
        uintptr_t target = 0
    ) {
        Resolve();

        size_t hook_size = HookType::Size(sig->hook_type);
        ASSERT(!hook_done);
        ASSERT(hook_size <= sig->patch_size);

        // Install the hook, linking to the given address.
        switch(sig->hook_type) {
            case HookType::Branch5:
                ASSERT(g_branchTrampoline.Write5Branch(real_address, target));
                break;
            case HookType::Branch6:
                ASSERT(g_branchTrampoline.Write6Branch(real_address, target));
                break;
            case HookType::Call5:
                ASSERT(g_branchTrampoline.Write5Call(real_address, target));
                break;
            case HookType::Call6:
                ASSERT(g_branchTrampoline.Write6Call(real_address, target));
                break;
            case HookType::DirectCall:
                ASSERT(SafeWriteCall(real_address, target));
                break;
            case HookType::DirectJump:
                ASSERT(SafeWriteJump(real_address, target));
                break;
            case HookType::Nop:
                break;
            default:
                HALT("Cannot install a hook with an invalid type");
        }

        // Overwrite the rest of the instruction with NOPs. We do this with
        // every hook to ensure the best compatibility with other SKSE
        // plugins.
        SafeMemSet(GetRetAddr(), kNop, sig->patch_size - hook_size);

        hook_done = true;
    }
};

#endif /* __SKYRIM_UNCAPPER_AE_RELOC_PATCH_H__ */