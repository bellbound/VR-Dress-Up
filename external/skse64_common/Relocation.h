#pragma once
// Compatibility shim: Maps old SKSE64 RelocPtr/RelocAddr to CommonLibSSE-NG's REL::Relocation

#include <REL/Relocation.h>

// RelocPtr<T> - pointer to data at a fixed offset from module base
template<typename T>
class RelocPtr
{
public:
    RelocPtr(std::uintptr_t offset) : m_offset(offset) {}

    operator T*() const {
        return reinterpret_cast<T*>(REL::Module::get().base() + m_offset);
    }

    T* operator->() const {
        return reinterpret_cast<T*>(REL::Module::get().base() + m_offset);
    }

    T& operator*() const {
        return *reinterpret_cast<T*>(REL::Module::get().base() + m_offset);
    }

    T* GetPtr() const {
        return reinterpret_cast<T*>(REL::Module::get().base() + m_offset);
    }

    std::uintptr_t GetUIntPtr() const {
        return REL::Module::get().base() + m_offset;
    }

private:
    std::uintptr_t m_offset;
};

// RelocAddr<T> - function pointer at a fixed offset from module base
template<typename T>
class RelocAddr
{
public:
    RelocAddr(std::uintptr_t offset) : m_offset(offset) {}

    operator T() const {
        return reinterpret_cast<T>(REL::Module::get().base() + m_offset);
    }

    std::uintptr_t GetUIntPtr() const {
        return REL::Module::get().base() + m_offset;
    }

private:
    std::uintptr_t m_offset;
};
