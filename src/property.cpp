#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "client.hpp"
#include "container.hpp"
#include "network.hpp"
#include "statistics.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include <sstream>

extern "C" {
#include <sys/sysinfo.h>
}

__thread TContainer *CurrentContainer = nullptr;
std::map<std::string, TProperty*> ContainerProperties;

TProperty::TProperty(std::string name, EProperty prop, std::string desc) {
    Name = name;
    Prop = prop;
    Desc = desc;
    ContainerProperties[name] = this;
}

TError TProperty::Set(const std::string &value) {
    if (IsReadOnly)
        return TError(EError::InvalidValue, "Read-only value: " + Name);
    return TError(EError::NotSupported, "Not implemented: " + Name);
}

TError TProperty::GetIndexed(const std::string &index, std::string &value) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

TError TProperty::SetIndexed(const std::string &index, const std::string &value) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

TError TProperty::GetToSave(std::string &value) {
    if (Prop != EProperty::NONE)
        return Get(value);
    return TError(EError::Unknown, "Trying to save non-serializable value");
}

TError TProperty::SetFromRestore(const std::string &value) {
    if (Prop != EProperty::NONE)
        return Set(value);
    return TError(EError::Unknown, "Trying to restore non-serializable value");
}

/*
 * Note for properties:
 * Dead state 2-line check is mandatory for all properties
 * Some properties require to check if the property is supported
 * Some properties may forbid changing it in runtime
 * Of course, some properties can be read-only
 */

TError TProperty::IsAliveAndStopped(void) {
    if (CurrentContainer->State != EContainerState::Stopped)
        return TError(EError::InvalidState, "Cannot change property for not stopped container");
    return TError::Success();
}

TError TProperty::IsAlive(void) {
    if (CurrentContainer->State == EContainerState::Dead)
        return TError(EError::InvalidState, "Cannot change property while in the dead state");
    return TError::Success();
}

TError TProperty::IsDead(void) {
    if (CurrentContainer->State != EContainerState::Dead)
        return TError(EError::InvalidState, "Available only in dead state: " + Name);
    return TError::Success();
}

TError TProperty::IsRunning(void) {
    /*
     * This snippet is taken from TContainer::GetProperty.
     * The method name misguides a bit, but may be the semantic
     * of such properties is that we can look at the value in
     * the dead state too...
     */
    if (CurrentContainer->State == EContainerState::Stopped)
        return TError(EError::InvalidState, "Not available in stopped state: " + Name);
    return TError::Success();
}

TError TProperty::WantControllers(uint64_t controllers) const {
    if (CurrentContainer->State == EContainerState::Stopped) {
        CurrentContainer->Controllers |= controllers;
        CurrentContainer->RequiredControllers |= controllers;
    } else if ((CurrentContainer->Controllers & controllers) != controllers)
        return TError(EError::NotSupported, "Cannot enable controllers in runtime");
    return TError::Success();
}

class TCapLimit : public TProperty {
public:
    TCapLimit() : TProperty(P_CAPABILITIES, EProperty::CAPABILITIES,
            "Limit capabilities in container: SYS_ADMIN;NET_ADMIN;... see man capabilities") {}

    TError CommitLimit(TCapabilities &limit) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        if (limit.Permitted & ~AllCapabilities.Permitted) {
            limit.Permitted &= ~AllCapabilities.Permitted;
            return TError(EError::InvalidValue,
                          "Unsupported capability: " + limit.Format());
        }

        TCapabilities bound;
        if (CurrentClient->IsSuperUser())
            bound = AllCapabilities;
        else if (CurrentContainer->VirtMode == VIRT_MODE_OS)
            bound = OsModeCapabilities;
        else
            bound = SuidCapabilities;

        /* host root user can allow any capabilities in its own containers */
        if (!CurrentClient->IsSuperUser() || !CurrentContainer->OwnerCred.IsRootUser()) {
            for (auto p = CurrentContainer->GetParent(); p; p = p->GetParent())
                bound.Permitted &= p->CapLimit.Permitted;
        }

        if (limit.Permitted & ~bound.Permitted) {
            limit.Permitted &= ~bound.Permitted;
            return TError(EError::Permission,
                          "Not allowed capability: " + limit.Format() +
                          ", you can set only: " + bound.Format());
        }

        CurrentContainer->CapLimit = limit;
        CurrentContainer->SetProp(EProperty::CAPABILITIES);
        CurrentContainer->SanitizeCapabilities();
        return TError::Success();
    }

    TError Get(std::string &value) {
        value = CurrentContainer->CapLimit.Format();
        return TError::Success();
    }

    TError Set(const std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(value);
        if (error)
            return error;
        return CommitLimit(caps);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(index);
        if (error)
            return error;
        value = BoolToString((CurrentContainer->CapLimit.Permitted &
                              caps.Permitted) == caps.Permitted);
        return TError::Success();
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TCapabilities caps;
        bool val;

        TError error = caps.Parse(index);
        if (!error)
            error = StringToBool(value, val);
        if (error)
            return error;
        if (val)
            caps.Permitted = CurrentContainer->CapLimit.Permitted | caps.Permitted;
        else
            caps.Permitted = CurrentContainer->CapLimit.Permitted & ~caps.Permitted;
        return CommitLimit(caps);
    }
} static Capabilities;

class TCapAmbient : public TProperty {
public:
    TCapAmbient() : TProperty(P_CAPABILITIES_AMBIENT, EProperty::CAPABILITIES_AMBIENT,
            "Raise capabilities in container: NET_BIND_SERVICE;SYS_PTRACE;...") {}

    void Init(void) {
        IsSupported = HasAmbientCapabilities;
    }

    TError CommitAmbient(TCapabilities &ambient) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        if (ambient.Permitted & ~AllCapabilities.Permitted) {
            ambient.Permitted &= ~AllCapabilities.Permitted;
            return TError(EError::InvalidValue,
                          "Unsupported capability: " + ambient.Format());
        }

        /* check allowed ambient capabilities */
        TCapabilities limit = CurrentContainer->CapAllowed;
        if (ambient.Permitted & ~limit.Permitted &&
                !CurrentClient->IsSuperUser()) {
            ambient.Permitted &= ~limit.Permitted;
            return TError(EError::Permission,
                          "Not allowed capability: " + ambient.Format() +
                          ", you can set only: " + limit.Format());
        }

        /* try to raise capabilities limit if required */
        limit = CurrentContainer->CapLimit;
        if (ambient.Permitted & ~limit.Permitted) {
            limit.Permitted |= ambient.Permitted;
            error = Capabilities.CommitLimit(limit);
            if (error)
                return error;
        }

        CurrentContainer->CapAmbient = ambient;
        CurrentContainer->SetProp(EProperty::CAPABILITIES_AMBIENT);
        CurrentContainer->SanitizeCapabilities();
        return TError::Success();
    }

    TError Get(std::string &value) {
        value = CurrentContainer->CapAmbient.Format();
        return TError::Success();
    }

    TError Set(const std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(value);
        if (error)
            return error;
        return CommitAmbient(caps);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(index);
        if (error)
            return error;
        value = BoolToString((CurrentContainer->CapAmbient.Permitted &
                              caps.Permitted) == caps.Permitted);
        return TError::Success();
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TCapabilities caps;
        bool val;

        TError error = caps.Parse(index);
        if (!error)
            error = StringToBool(value, val);
        if (error)
            return error;
        if (val)
            caps.Permitted = CurrentContainer->CapAmbient.Permitted | caps.Permitted;
        else
            caps.Permitted = CurrentContainer->CapAmbient.Permitted & ~caps.Permitted;
        return CommitAmbient(caps);
    }
} static CapabilitiesAmbient;

class TCwd : public TProperty {
public:
    TCwd() : TProperty(P_CWD, EProperty::CWD, "Container working directory") {}
    TError Get(std::string &value) {
        value = CurrentContainer->GetCwd();
        return TError::Success();
    }
    TError Set(const std::string &cwd) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CurrentContainer->Cwd = cwd;
        CurrentContainer->SetProp(EProperty::CWD);
        return TError::Success();
    }
} static Cwd;

class TUlimit : public TProperty {
public:
    TUlimit() : TProperty(P_ULIMIT, EProperty::ULIMIT,
            "Process limits: as|core|data|locks|memlock|nofile|nproc|stack: [soft]|unlimited [hard];... (see man prlimit) (dynamic)") {}

    TError Get(std::string &value) {
        value = StringMapToString(CurrentContainer->Ulimit);
        return TError::Success();
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto it = CurrentContainer->Ulimit.find(index);
        if (it != CurrentContainer->Ulimit.end())
            value = it->second;
        else
            value = "";
        return TError::Success();
    }

    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        TStringMap map;
        error = StringToStringMap(value, map);
        if (error)
            return error;

        for (auto &it: map) {
            int res;
            struct rlimit lim;

            error = ParseUlimit(it.first, it.second, res, lim);
            if (error)
                return error;
        }

        CurrentContainer->Ulimit = map;
        CurrentContainer->SetProp(EProperty::ULIMIT);
        return TError::Success();
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TError error;
        int res;
        struct rlimit lim;

        if (value == "") {
            CurrentContainer->Ulimit.erase(index);
        } else {
            error = ParseUlimit(index, value, res, lim);
            if (error)
                return error;
            CurrentContainer->Ulimit[index] = value;
        }
        CurrentContainer->SetProp(EProperty::ULIMIT);
        return TError::Success();
    }

} static Ulimit;

class TCpuPolicy : public TProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TCpuPolicy() : TProperty(P_CPU_POLICY, EProperty::CPU_POLICY,
            "CPU policy: rt, high, normal, batch, idle (dynamic)") {}
} static CpuPolicy;

TError TCpuPolicy::Set(const std::string &policy) {
    TError error = IsAlive();
    if (error)
        return error;

    if (policy != "rt" && policy != "high" && policy != "normal" &&
            policy != "batch"  && policy != "idle" && policy != "iso")
        return TError(EError::InvalidValue, "Unknown cpu policy: " + policy);

    if (CurrentContainer->CpuPolicy != policy) {
        CurrentContainer->CpuPolicy = policy;
        CurrentContainer->SetProp(EProperty::CPU_POLICY);

        CurrentContainer->SchedPolicy = SCHED_OTHER;
        CurrentContainer->SchedPrio = 0;
        CurrentContainer->SchedNice = 0;

        if (policy == "rt") {
            CurrentContainer->SchedNice = config().container().rt_nice();
            if ((!CpuSubsystem.HasSmart ||
                 !config().container().enable_smart()) &&
                    config().container().rt_priority()) {
                CurrentContainer->SchedPolicy = SCHED_RR;
                CurrentContainer->SchedPrio = config().container().rt_priority();
            }
        } else if (policy == "high") {
            CurrentContainer->SchedNice = config().container().high_nice();
        } else if (policy == "batch") {
            CurrentContainer->SchedPolicy = SCHED_BATCH;
        } else if (policy == "idle") {
            CurrentContainer->SchedPolicy = SCHED_IDLE;
        } else if (policy == "iso") {
            CurrentContainer->SchedPolicy = 4;
            CurrentContainer->SchedNice = config().container().high_nice();
        }
    }

    return TError::Success();
}

TError TCpuPolicy::Get(std::string &value) {
    value = CurrentContainer->CpuPolicy;

    return TError::Success();
}

class TIoPolicy : public TProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TIoPolicy() : TProperty(P_IO_POLICY, EProperty::IO_POLICY,
                            "IO policy: normal | batch (dynamic)") {}
    void Init(void) {
        IsSupported = BlkioSubsystem.HasWeight;
    }
} static IoPolicy;

TError TIoPolicy::Set(const std::string &policy) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_BLKIO);
    if (error)
        return error;

    if (policy != "normal" && policy != "batch")
        return TError(EError::InvalidValue, "invalid policy: " + policy);

    if (CurrentContainer->IoPolicy != policy) {
        CurrentContainer->IoPolicy = policy;
        CurrentContainer->SetProp(EProperty::IO_POLICY);
    }

    return TError::Success();
}

TError TIoPolicy::Get(std::string &value) {
    value = CurrentContainer->IoPolicy;

    return TError::Success();
}

class TUser : public TProperty {
public:
    TUser() : TProperty(P_USER, EProperty::USER, "Start command with given user") {}

    TError Get(std::string &value) {
        value = UserName(CurrentContainer->TaskCred.Uid);
        return TError::Success();
    }

    TError Set(const std::string &username) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        TCred newCred;
        gid_t oldGid = CurrentContainer->TaskCred.Gid;
        error = newCred.Load(username);

        /* allow any numeric id if client can change uid/gid */
        if (error && CurrentClient->CanSetUidGid()) {
            newCred.Gid = oldGid;
            error = UserId(username, newCred.Uid);
        }

        if (error)
            return error;

        if (newCred.Uid == CurrentContainer->TaskCred.Uid)
            return TError::Success();

        /* try to preserve current group if possible */
        if (newCred.IsMemberOf(oldGid) ||
                CurrentClient->Cred.IsMemberOf(oldGid) ||
                CurrentClient->IsSuperUser())
            newCred.Gid = oldGid;

        error = CurrentClient->CanControl(newCred);

        /* allow any user in sub-container if client can change uid/gid */
        if (error && CurrentClient->CanSetUidGid() &&
                CurrentContainer->IsChildOf(*CurrentClient->ClientContainer))
            error = TError::Success();

        if (error)
            return error;

        CurrentContainer->TaskCred = newCred;
        CurrentContainer->SetProp(EProperty::USER);
        return TError::Success();
    }
} static User;

class TGroup : public TProperty {
public:
    TGroup() : TProperty(P_GROUP, EProperty::GROUP, "Start command with given group") {}

    TError Get(std::string &value) {
        value = GroupName(CurrentContainer->TaskCred.Gid);
        return TError::Success();
    }

    TError Set(const std::string &groupname) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        gid_t newGid;
        error = GroupId(groupname, newGid);
        if (error)
            return error;

        if (!CurrentContainer->TaskCred.IsMemberOf(newGid) &&
                !CurrentClient->Cred.IsMemberOf(newGid) &&
                !CurrentClient->IsSuperUser())
            error = TError(EError::Permission, "Desired group : " + groupname +
                           " isn't in current user supplementary group list");

        /* allow any group in sub-container if client can change uid/gid */
        if (error && CurrentClient->CanSetUidGid() &&
                CurrentContainer->IsChildOf(*CurrentClient->ClientContainer))
            error = TError::Success();

        if (error)
            return error;

        CurrentContainer->TaskCred.Gid = newGid;
        CurrentContainer->SetProp(EProperty::GROUP);
        return TError::Success();
    }
} static Group;

class TOwnerUser : public TProperty {
public:
 TOwnerUser() : TProperty(P_OWNER_USER, EProperty::OWNER_USER,
                          "Container owner user") {}

    TError Get(std::string &value) {
        value = UserName(CurrentContainer->OwnerCred.Uid);
        return TError::Success();
    }

    TError Set(const std::string &username) {
        TCred newCred;
        gid_t oldGid = CurrentContainer->OwnerCred.Gid;
        TError error = newCred.Load(username);
        if (error)
            return error;

        /* try to preserve current group if possible */
        if (newCred.IsMemberOf(oldGid) ||
                CurrentClient->Cred.IsMemberOf(oldGid) ||
                CurrentClient->IsSuperUser())
            newCred.Gid = oldGid;

        error = CurrentClient->CanControl(newCred);
        if (error)
            return error;

        CurrentContainer->OwnerCred = newCred;
        CurrentContainer->SetProp(EProperty::OWNER_USER);
        CurrentContainer->SanitizeCapabilities();
        return TError::Success();
    }
} static OwnerUser;

class TOwnerGroup : public TProperty {
public:
    TOwnerGroup() : TProperty(P_OWNER_GROUP, EProperty::OWNER_GROUP,
                              "Container owner group") {}

    TError Get(std::string &value) {
        value = GroupName(CurrentContainer->OwnerCred.Gid);
        return TError::Success();
    }

    TError Set(const std::string &groupname) {
        gid_t newGid;
        TError error = GroupId(groupname, newGid);
        if (error)
            return error;

        if (!CurrentContainer->OwnerCred.IsMemberOf(newGid) &&
                !CurrentClient->Cred.IsMemberOf(newGid) &&
                !CurrentClient->IsSuperUser())
            return TError(EError::Permission, "Desired group : " + groupname +
                    " isn't in current user supplementary group list");

        CurrentContainer->OwnerCred.Gid = newGid;
        CurrentContainer->SetProp(EProperty::OWNER_GROUP);
        return TError::Success();
    }
} static OwnerGroup;

class TMemoryGuarantee : public TProperty {
public:
    TError Set(const std::string &mem_guarantee);
    TError Get(std::string &value);
    TMemoryGuarantee() : TProperty(P_MEM_GUARANTEE, EProperty::MEM_GUARANTEE,
                                    "Guaranteed amount of memory "
                                    "[bytes] (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
} static MemoryGuarantee;

TError TMemoryGuarantee::Set(const std::string &mem_guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_val = 0lu;
    error = StringToSize(mem_guarantee, new_val);
    if (error)
        return error;

    CurrentContainer->NewMemGuarantee = new_val;

    uint64_t total = GetTotalMemory();
    uint64_t usage = RootContainer->GetTotalMemGuarantee();
    uint64_t reserve = config().daemon().memory_guarantee_reserve();

    if (usage + reserve > total) {
        CurrentContainer->NewMemGuarantee = CurrentContainer->MemGuarantee;
        int64_t left = total - reserve - RootContainer->GetTotalMemGuarantee();
        return TError(EError::ResourceNotAvailable, "Only " + std::to_string(left) + " bytes left");
    }

    if (CurrentContainer->MemGuarantee != new_val) {
        CurrentContainer->MemGuarantee = new_val;
        CurrentContainer->SetProp(EProperty::MEM_GUARANTEE);
    }

    return TError::Success();
}

TError TMemoryGuarantee::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MemGuarantee);

    return TError::Success();
}

class TMemTotalGuarantee : public TProperty {
public:
    TMemTotalGuarantee() : TProperty(P_MEM_TOTAL_GUARANTEE, EProperty::NONE,
                                     "Total amount of memory "
                                     "guaranteed for porto "
                                     "containers") {
        IsReadOnly = true;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
    TError Get(std::string &value) {
        value = std::to_string(CurrentContainer->GetTotalMemGuarantee());
        return TError::Success();
    }
} static MemTotalGuarantee;

class TCommand : public TProperty {
public:
    TCommand() : TProperty(P_COMMAND, EProperty::COMMAND,
                           "Command executed upon container start") {}
    TError Get(std::string &command) {
        command = CurrentContainer->Command;
        return TError::Success();
    }
    TError Set(const std::string &command) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CurrentContainer->Command = command;
        CurrentContainer->SetProp(EProperty::COMMAND);
        return TError::Success();
    }
} static Command;

class TVirtMode : public TProperty {
public:
    TError Set(const std::string &virt_mode);
    TError Get(std::string &value);
    TVirtMode() : TProperty(P_VIRT_MODE, EProperty::VIRT_MODE,
                            "Virtualization mode: os|app") {}
} static VirtMode;

TError TVirtMode::Set(const std::string &virt_mode) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (virt_mode == P_VIRT_MODE_APP)
        CurrentContainer->VirtMode = VIRT_MODE_APP;
    else if (virt_mode == P_VIRT_MODE_OS)
        CurrentContainer->VirtMode = VIRT_MODE_OS;
    else
        return TError(EError::InvalidValue, std::string("Unsupported ") +
                      P_VIRT_MODE + ": " + virt_mode);

    CurrentContainer->SetProp(EProperty::VIRT_MODE);
    CurrentContainer->SanitizeCapabilities();

    return TError::Success();
}

TError TVirtMode::Get(std::string &value) {

    switch (CurrentContainer->VirtMode) {
        case VIRT_MODE_APP:
            value = P_VIRT_MODE_APP;
            break;
        case VIRT_MODE_OS:
            value = P_VIRT_MODE_OS;
            break;
        default:
            value = "unknown " + std::to_string(CurrentContainer->VirtMode);
            break;
    }

    return TError::Success();
}

class TStdinPath : public TProperty {
public:
    TStdinPath() : TProperty(P_STDIN_PATH, EProperty::STDIN,
            "Container standard input path") {}
    TError Get(std::string &value) {
        value = CurrentContainer->Stdin.Path.ToString();
        return TError::Success();
    }
    TError Set(const std::string &path) {
        TError error = IsAliveAndStopped();
        if (!error) {
            CurrentContainer->Stdin.SetInside(path);
            CurrentContainer->SetProp(EProperty::STDIN);
        }
        return error;
    }

} static StdinPath;

class TStdoutPath : public TProperty {
public:
    TStdoutPath() : TProperty(P_STDOUT_PATH, EProperty::STDOUT,
            "Container standard output path") {}
    TError Get(std::string &value) {
        value =  CurrentContainer->Stdout.Path.ToString();
        return TError::Success();
    }
    TError Set(const std::string &path) {
        TError error = IsAliveAndStopped();
        if (!error) {
            CurrentContainer->Stdout.SetInside(path);
            CurrentContainer->SetProp(EProperty::STDOUT);
        }
        return error;
    }
} static StdoutPath;

class TStderrPath : public TProperty {
public:
    TStderrPath() : TProperty(P_STDERR_PATH, EProperty::STDERR,
            "Container standard error path") {}
    TError Get(std::string &value) {
        value = CurrentContainer->Stderr.Path.ToString();
        return TError::Success();
    }
    TError Set(const std::string &path) {
        TError error = IsAliveAndStopped();
        if (!error) {
            CurrentContainer->Stderr.SetInside(path);
            CurrentContainer->SetProp(EProperty::STDERR);
        }
        return error;
    }
} static StderrPath;

class TStdoutLimit : public TProperty {
public:
    TStdoutLimit() : TProperty(P_STDOUT_LIMIT, EProperty::STDOUT_LIMIT,
            "Limit for stored stdout and stderr size (dynamic)") {}
    TError Get(std::string &value) {
        value = std::to_string(CurrentContainer->Stdout.Limit);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        uint64_t limit;
        TError error = StringToSize(value, limit);
        if (error)
            return error;

        uint64_t limit_max = config().container().stdout_limit_max();
        if (limit > limit_max && !CurrentClient->IsSuperUser())
            return TError(EError::Permission,
                          "Maximum limit is: " + std::to_string(limit_max));

        CurrentContainer->Stdout.Limit = limit;
        CurrentContainer->Stderr.Limit = limit;
        CurrentContainer->SetProp(EProperty::STDOUT_LIMIT);
        return TError::Success();
    }
} static StdoutLimit;

class TStdoutOffset : public TProperty {
public:
    TStdoutOffset() : TProperty(D_STDOUT_OFFSET, EProperty::NONE,
            "Offset of stored stdout (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        value = std::to_string(CurrentContainer->Stdout.Offset);
        return TError::Success();
    }
} static StdoutOffset;

class TStderrOffset : public TProperty {
public:
    TStderrOffset() : TProperty(D_STDERR_OFFSET, EProperty::NONE,
            "Offset of stored stderr (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        value = std::to_string(CurrentContainer->Stderr.Offset);
        return TError::Success();
    }
} static StderrOffset;

class TStdout : public TProperty {
public:
    TStdout() : TProperty(D_STDOUT, EProperty::NONE,
            "stdout [[offset][:length]] (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CurrentContainer->Stdout.Read(*CurrentContainer, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CurrentContainer->Stdout.Read(*CurrentContainer, value, index);
    }
} static Stdout;

class TStderr : public TProperty {
public:
    TStderr() : TProperty(D_STDERR, EProperty::NONE,
            "stderr [[offset][:length]] (ro))") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CurrentContainer->Stderr.Read(*CurrentContainer, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CurrentContainer->Stderr.Read(*CurrentContainer, value, index);
    }
} static Stderr;

class TBindDns : public TProperty {
public:
    TBindDns() : TProperty(P_BIND_DNS, EProperty::BIND_DNS,
                           "Bind /etc/resolv.conf and /etc/hosts"
                           " from host into container root") {}
    TError Get(std::string &value) {
        value = BoolToString(CurrentContainer->BindDns);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        error = StringToBool(value, CurrentContainer->BindDns);
        if (error)
            return error;
        CurrentContainer->SetProp(EProperty::BIND_DNS);
        return TError::Success();
    }
} static BindDns;


class TIsolate : public TProperty {
public:
    TError Set(const std::string &isolate_needed);
    TError Get(std::string &value);
    TIsolate() : TProperty(P_ISOLATE, EProperty::ISOLATE,
                           "Isolate container from parent") {}
} static Isolate;

TError TIsolate::Get(std::string &value) {
    value = CurrentContainer->Isolate ? "true" : "false";

    return TError::Success();
}

TError TIsolate::Set(const std::string &isolate_needed) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (isolate_needed == "true")
        CurrentContainer->Isolate = true;
    else if (isolate_needed == "false")
        CurrentContainer->Isolate = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::ISOLATE);

    return TError::Success();
}

class TRoot : public TProperty {
public:
    TError Set(const std::string &root);
    TError Get(std::string &value);
    TRoot() : TProperty(P_ROOT, EProperty::ROOT, "Container root directory"
                        "(container will be chrooted into ths directory)") {}
} static Root;

TError TRoot::Get(std::string &value) {
    value = CurrentContainer->Root;

    return TError::Success();
}

TError TRoot::Set(const std::string &root) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Root = root;
    CurrentContainer->SetProp(EProperty::ROOT);

    return TError::Success();
}

class TNet : public TProperty {
public:
    TError Set(const std::string &net_desc);
    TError Get(std::string &value);
    TNet() : TProperty(P_NET, EProperty::NET,
 "Container network settings: "
 "none | "
 "inherited (default) | "
 "steal <name> | "
 "container <name> | "
 "macvlan <master> <name> [bridge|private|vepa|passthru] [mtu] [hw] | "
 "ipvlan <master> <name> [l2|l3] [mtu] | "
 "veth <name> <bridge> [mtu] [hw] | "
 "L3 <name> [master] | "
 "NAT [name] | "
 "MTU <name> <mtu> | "
 "autoconf <name> (SLAAC) | "
 "netns <name>") {}
} static Net;

TError TNet::Set(const std::string &net_desc) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TMultiTuple new_net_desc;
    SplitEscapedString(net_desc, new_net_desc, ' ', ';');

    TNetCfg cfg;
    error = cfg.ParseNet(new_net_desc);
    if (error)
        return error;

    if (!cfg.Inherited) {
        error = WantControllers(CGROUP_NETCLS);
        if (error)
            return error;
    }

    CurrentContainer->NetProp = new_net_desc; /* FIXME: Copy vector contents? */

    CurrentContainer->SetProp(EProperty::NET);
    return TError::Success();
}

TError TNet::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->NetProp, ' ', ';');
    return TError::Success();
}

class TRootRo : public TProperty {
public:
    TError Set(const std::string &ro);
    TError Get(std::string &value);
    TRootRo() : TProperty(P_ROOT_RDONLY, EProperty::ROOT_RDONLY,
                          "Mount root directory in read-only mode") {}
} static RootRo;

TError TRootRo::Set(const std::string &ro) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (ro == "true")
        CurrentContainer->RootRo = true;
    else if (ro == "false")
        CurrentContainer->RootRo = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::ROOT_RDONLY);

    return TError::Success();
}

TError TRootRo::Get(std::string &ro) {
    ro = CurrentContainer->RootRo ? "true" : "false";

    return TError::Success();
}

class TUmask : public TProperty {
public:
    TUmask() : TProperty(P_UMASK, EProperty::UMASK, "Set file mode creation mask") { }
    TError Get(std::string &value) {
        value = StringFormat("%#o", CurrentContainer->Umask);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        error = StringToOct(value, CurrentContainer->Umask);
        if (error)
            return error;
        CurrentContainer->SetProp(EProperty::UMASK);
        return TError::Success();
    }
} static Umask;

class TControllers : public TProperty {
public:
    TControllers() : TProperty(P_CONTROLLERS, EProperty::CONTROLLERS, "Cgroup controllers") { }
    TError Get(std::string &value) {
        value = StringFormatFlags(CurrentContainer->Controllers, ControllersName, ";");
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        uint64_t val;
        error = StringParseFlags(value, ControllersName, val, ';');
        if (error)
            return error;
        if ((val & CurrentContainer->RequiredControllers) != CurrentContainer->RequiredControllers)
            return TError(EError::InvalidValue, "Cannot disable required controllers");
        CurrentContainer->Controllers = val;
        CurrentContainer->SetProp(EProperty::CONTROLLERS);
        return TError::Success();
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        uint64_t val;
        TError error = StringParseFlags(index, ControllersName, val, ';');
        if (error)
            return error;
        value = BoolToString((CurrentContainer->Controllers & val) == val);
        return TError::Success();
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        uint64_t val;
        bool enable;
        TError error = StringParseFlags(index, ControllersName, val, ';');
        if (error)
            return error;
        error = StringToBool(value, enable);
        if (error)
            return error;
        if (enable)
            val = CurrentContainer->Controllers | val;
        else
            val = CurrentContainer->Controllers & ~val;
        if ((val & CurrentContainer->RequiredControllers) != CurrentContainer->RequiredControllers)
            return TError(EError::InvalidValue, "Cannot disable required controllers");
        CurrentContainer->Controllers = val;
        CurrentContainer->SetProp(EProperty::CONTROLLERS);
        return TError::Success();
    }
} static Controllers;

class TCgroups : public TProperty {
public:
    TCgroups() : TProperty(D_CGROUPS, EProperty::NONE, "Cgroups") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        TStringMap map;
        for (auto &subsys: Subsystems)
            map[subsys->Type] = CurrentContainer->GetCgroup(*subsys).Path().ToString();
        value = StringMapToString(map);
        return TError::Success();
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        for (auto &subsys: Subsystems) {
            if (subsys->Type != index)
                continue;
            value = CurrentContainer->GetCgroup(*subsys).Path().ToString();
            return TError::Success();
        }
        return TError(EError::InvalidProperty, "Unknown cgroup subststem: " + index);
    }
} static Cgroups;

class THostname : public TProperty {
public:
    TError Set(const std::string &hostname);
    TError Get(std::string &value);
    THostname() : TProperty(P_HOSTNAME, EProperty::HOSTNAME, "Container hostname") {}
} static Hostname;

TError THostname::Set(const std::string &hostname) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Hostname = hostname;
    CurrentContainer->SetProp(EProperty::HOSTNAME);

    return TError::Success();
}

TError THostname::Get(std::string &value) {
    value = CurrentContainer->Hostname;

    return TError::Success();
}

class TEnvProperty : public TProperty {
public:
    TError Set(const std::string &env);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TError SetIndexed(const std::string &index, const std::string &env_val);
    TEnvProperty() : TProperty(P_ENV, EProperty::ENV,
                       "Container environment variables: <name>=<value>; ...") {}
} static EnvProperty;

TError TEnvProperty::Set(const std::string &env_val) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TTuple envs;
    SplitEscapedString(env_val, envs, ';');

    TEnv env;
    error =  env.Parse(envs, true);
    if (error)
        return error;

    env.Format(CurrentContainer->EnvCfg);
    CurrentContainer->SetProp(EProperty::ENV);

    return TError::Success();
}

TError TEnvProperty::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->EnvCfg, ';');
    return TError::Success();
}

TError TEnvProperty::SetIndexed(const std::string &index, const std::string &env_val) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TEnv env;
    error = env.Parse(CurrentContainer->EnvCfg, true);
    if (error)
        return error;

    error = env.Parse({index + "=" + env_val}, true);
    if (error)
        return error;

    env.Format(CurrentContainer->EnvCfg);
    CurrentContainer->SetProp(EProperty::ENV);

    return TError::Success();
}

TError TEnvProperty::GetIndexed(const std::string &index, std::string &value) {
    TEnv env;
    TError error = CurrentContainer->GetEnvironment(env);
    if (error)
        return error;

    if (!env.GetEnv(index, value))
        return TError(EError::InvalidValue, "Variable " + index + " not defined");

    return TError::Success();
}

class TBind : public TProperty {
public:
    TError Set(const std::string &bind_str);
    TError Get(std::string &value);
    TBind() : TProperty(P_BIND, EProperty::BIND,
                        "Share host directories with container: "
                        "<host_path> <container_path> [ro|rw]; ...") {}
} static Bind;

TError TBind::Set(const std::string &bind_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TMultiTuple binds;
    SplitEscapedString(bind_str, binds, ' ', ';');

    std::vector<TBindMount> bindMounts;

    for (auto &bind : binds) {
        TBindMount bm;

        if (bind.size() != 2 && bind.size() != 3)
            return TError(EError::InvalidValue, "Invalid bind in: " +
                          MergeEscapeStrings(bind, ' '));

        bm.Source = bind[0];
        bm.Dest = bind[1];
        bm.ReadOnly = false;
        bm.ReadWrite = false;

        if (bind.size() == 3) {
            if (bind[2] == "ro")
                bm.ReadOnly = true;
            else if (bind[2] == "rw")
                bm.ReadWrite = true;
            else
                return TError(EError::InvalidValue, "Invalid bind type in: " +
                              MergeEscapeStrings(bind, ' '));
        }

        bindMounts.push_back(bm);
    }

    CurrentContainer->BindMounts = bindMounts;
    CurrentContainer->SetProp(EProperty::BIND);

    return TError::Success();
}

TError TBind::Get(std::string &value) {
    TMultiTuple tuples;
    for (const auto &bm : CurrentContainer->BindMounts) {
        tuples.push_back({ bm.Source.ToString(), bm.Dest.ToString() });

        if (bm.ReadOnly)
            tuples.back().push_back("ro");
        else if (bm.ReadWrite)
            tuples.back().push_back("rw");
    }
    value = MergeEscapeStrings(tuples, ' ', ';');
    return TError::Success();
}

class TIp : public TProperty {
public:
    TError Set(const std::string &ipaddr);
    TError Get(std::string &value);
    TIp() : TProperty(P_IP, EProperty::IP,
                      "IP configuration: <interface> <ip>/<prefix>; ...") {}
} static Ip;

TError TIp::Set(const std::string &ipaddr) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TMultiTuple ipaddrs;
    SplitEscapedString(ipaddr, ipaddrs, ' ', ';');

    TNetCfg cfg;
    error = cfg.ParseIp(ipaddrs);
    if (error)
        return error;

    CurrentContainer->IpList = ipaddrs;
    CurrentContainer->SetProp(EProperty::IP);

    return TError::Success();
}

TError TIp::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->IpList, ' ', ';');
    return TError::Success();
}

class TDefaultGw : public TProperty {
public:
    TError Set(const std::string &gw);
    TError Get(std::string &value);
    TDefaultGw() : TProperty(P_DEFAULT_GW, EProperty::DEFAULT_GW,
            "Default gateway: <interface> <ip>; ...") {}
} static DefaultGw;

TError TDefaultGw::Set(const std::string &gw) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TNetCfg cfg;
    TMultiTuple gws;
    SplitEscapedString(gw, gws, ' ', ';');

    error = cfg.ParseGw(gws);
    if (error)
        return error;

    CurrentContainer->DefaultGw = gws;
    CurrentContainer->SetProp(EProperty::DEFAULT_GW);

    return TError::Success();
}

TError TDefaultGw::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->DefaultGw, ' ', ';');
    return TError::Success();
}

class TResolvConf : public TProperty {
public:
    TError Set(const std::string &conf);
    TError Get(std::string &value);
    TResolvConf() : TProperty(P_RESOLV_CONF, EProperty::RESOLV_CONF,
                              "DNS resolver configuration: "
                              "<resolv.conf option>;...") {}
} static ResolvConf;

TError TResolvConf::Set(const std::string &conf_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TTuple conf;
    SplitEscapedString(conf_str, conf, ';');

    CurrentContainer->ResolvConf = conf;
    CurrentContainer->SetProp(EProperty::RESOLV_CONF);

    return TError::Success();
}

TError TResolvConf::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->ResolvConf, ';');
    return TError::Success();
}

class TDevices : public TProperty {
public:
    TDevices() : TProperty(P_DEVICES, EProperty::DEVICES,
                                   "Devices that container can access: "
                                   "<device> [r][w][m][-] [name] [mode] "
                                   "[user] [group]; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CurrentContainer->Devices, ' ', ';');
        return TError::Success();
    }
    TError Set(const std::string &dev) {
        TError error = WantControllers(CGROUP_DEVICES);
        if (error)
            return error;

        TMultiTuple dev_list;

        SplitEscapedString(dev, dev_list, ' ', ';');
        CurrentContainer->Devices = dev_list;
        CurrentContainer->SetProp(EProperty::DEVICES);

        return TError::Success();
    }
} static Devices;

class TRawRootPid : public TProperty {
public:
    TRawRootPid() : TProperty(P_RAW_ROOT_PID, EProperty::ROOT_PID, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = StringFormat("%d;%d;%d", CurrentContainer->Task.Pid,
                                         CurrentContainer->TaskVPid,
                                         CurrentContainer->WaitTask.Pid);
        return TError::Success();
    }
    TError SetFromRestore(const std::string &value) {
        std::vector<std::string> val;
        TError error;

        SplitEscapedString(value, val, ';');
        if (val.size() > 0)
            error = StringToInt(val[0], CurrentContainer->Task.Pid);
        else
            CurrentContainer->Task.Pid = 0;
        if (!error && val.size() > 1)
            error = StringToInt(val[1], CurrentContainer->TaskVPid);
        else
            CurrentContainer->TaskVPid = 0;
        if (!error && val.size() > 2)
            error = StringToInt(val[2], CurrentContainer->WaitTask.Pid);
        else
            CurrentContainer->WaitTask.Pid = CurrentContainer->Task.Pid;
        return error;
    }
} static RawRootPid;

class TSeizePid : public TProperty {
public:
    TSeizePid() : TProperty(P_SEIZE_PID, EProperty::SEIZE_PID, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CurrentContainer->SeizeTask.Pid);
        return TError::Success();
    }
    TError SetFromRestore(const std::string &value) {
        return StringToInt(value, CurrentContainer->SeizeTask.Pid);
    }
} static SeizePid;

class TRawLoopDev : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRawLoopDev() : TProperty(P_RAW_LOOP_DEV, EProperty::LOOP_DEV, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static RawLoopDev;

TError TRawLoopDev::SetFromRestore(const std::string &value) {
    return StringToInt(value, CurrentContainer->LoopDev);
}

TError TRawLoopDev::Get(std::string &value) {
    value = std::to_string(CurrentContainer->LoopDev);

    return TError::Success();
}

class TRawStartTime : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRawStartTime() : TProperty(P_RAW_START_TIME, EProperty::START_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static RawStartTime;

TError TRawStartTime::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CurrentContainer->StartTime);
}

TError TRawStartTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->StartTime);

    return TError::Success();
}

class TRawDeathTime : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRawDeathTime() : TProperty(P_RAW_DEATH_TIME, EProperty::DEATH_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static RawDeathTime;

TError TRawDeathTime::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CurrentContainer->DeathTime);
}

TError TRawDeathTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->DeathTime);

    return TError::Success();
}

class TPortoNamespace : public TProperty {
public:
    TPortoNamespace() : TProperty(P_PORTO_NAMESPACE, EProperty::PORTO_NAMESPACE,
            "Porto containers namespace (container name prefix)") {}
    TError Get(std::string &value) {
        value = CurrentContainer->NsName;
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CurrentContainer->NsName = value;
        CurrentContainer->SetProp(EProperty::PORTO_NAMESPACE);
        return TError::Success();
    }
} static PortoNamespace;

class TMemoryLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TMemoryLimit() : TProperty(P_MEM_LIMIT, EProperty::MEM_LIMIT,
                               "Memory hard limit [bytes] (dynamic)") {}
} static MemoryLimit;

TError TMemoryLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (new_size && new_size < config().container().min_memory_limit())
        return TError(EError::InvalidValue, "Should be at least " +
                std::to_string(config().container().min_memory_limit()));

    if (CurrentContainer->MemLimit != new_size) {
        CurrentContainer->MemLimit = new_size;
        CurrentContainer->SetProp(EProperty::MEM_LIMIT);
    }

    return TError::Success();
}

TError TMemoryLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MemLimit);

    return TError::Success();
}

class TAnonLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TAnonLimit() : TProperty(P_ANON_LIMIT, EProperty::ANON_LIMIT,
                             "Anonymous memory limit [bytes] (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();
    }

} static AnonLimit;

TError TAnonLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (new_size && new_size < config().container().min_memory_limit())
        return TError(EError::InvalidValue, "Should be at least " +
                std::to_string(config().container().min_memory_limit()));

    if (CurrentContainer->AnonMemLimit != new_size) {
        CurrentContainer->AnonMemLimit = new_size;
        CurrentContainer->SetProp(EProperty::ANON_LIMIT);
    }

    return TError::Success();
}

TError TAnonLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->AnonMemLimit);

    return TError::Success();
}

class TDirtyLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TDirtyLimit() : TProperty(P_DIRTY_LIMIT, EProperty::DIRTY_LIMIT,
                              "Dirty file cache limit [bytes] "
                              "(dynamic)" ) {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportDirtyLimit();
    }
} static DirtyLimit;

TError TDirtyLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (new_size && new_size < config().container().min_memory_limit())
        return TError(EError::InvalidValue, "Should be at least " +
                std::to_string(config().container().min_memory_limit()));

    if (CurrentContainer->DirtyMemLimit != new_size) {
        CurrentContainer->DirtyMemLimit = new_size;
        CurrentContainer->SetProp(EProperty::DIRTY_LIMIT);
    }

    return TError::Success();
}

TError TDirtyLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->DirtyMemLimit);

    return TError::Success();
}

class THugetlbLimit : public TProperty {
public:
    THugetlbLimit() : TProperty(P_HUGETLB_LIMIT, EProperty::HUGETLB_LIMIT,
                                "Hugetlb memory limit [bytes] (dynamic)") {}
    void Init(void) {
        IsSupported = HugetlbSubsystem.Supported;
    }
    TError Get(std::string &value) {
        if (CurrentContainer->HasProp(EProperty::HUGETLB_LIMIT))
            value = std::to_string(CurrentContainer->HugetlbLimit);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_HUGETLB);
        if (error)
            return error;
        if (value.empty()) {
            CurrentContainer->HugetlbLimit = -1;
            CurrentContainer->ClearProp(EProperty::HUGETLB_LIMIT);
        } else {
            uint64_t limit = 0lu;
            error = StringToSize(value, limit);
            if (error)
                return error;

            auto cg = CurrentContainer->GetCgroup(HugetlbSubsystem);
            uint64_t usage;
            if (!HugetlbSubsystem.GetHugeUsage(cg, usage) && limit < usage)
                return TError(EError::InvalidValue,
                              "current hugetlb usage is greater than limit");

            CurrentContainer->HugetlbLimit = limit;
            CurrentContainer->SetProp(EProperty::HUGETLB_LIMIT);
        }
        return TError::Success();
    }
} static HugetlbLimit;

class TRechargeOnPgfault : public TProperty {
public:
    TError Set(const std::string &recharge);
    TError Get(std::string &value);
    TRechargeOnPgfault() : TProperty(P_RECHARGE_ON_PGFAULT,
                                     EProperty::RECHARGE_ON_PGFAULT,
                                     "Recharge memory on "
                                     "page fault (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportRechargeOnPgfault();
    }
} static RechargeOnPgfault;

TError TRechargeOnPgfault::Set(const std::string &recharge) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    bool new_val;
    if (recharge == "true")
        new_val = true;
    else if (recharge == "false")
        new_val = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    if (CurrentContainer->RechargeOnPgfault != new_val) {
        CurrentContainer->RechargeOnPgfault = new_val;
        CurrentContainer->SetProp(EProperty::RECHARGE_ON_PGFAULT);
    }

    return TError::Success();
}

TError TRechargeOnPgfault::Get(std::string &value) {
    value = CurrentContainer->RechargeOnPgfault ? "true" : "false";

    return TError::Success();
}

class TCpuLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TCpuLimit() : TProperty(P_CPU_LIMIT, EProperty::CPU_LIMIT,
                            "CPU limit: 0-100.0 [%] | 0.0c-<CPUS>c "
                            " [cores] (dynamic)") {}
} static CpuLimit;

TError TCpuLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_CPU);
    if (error)
        return error;

    double new_limit;
    error = StringToCpuValue(limit, new_limit);
    if (error)
        return error;

    if (new_limit > CurrentContainer->Parent->CpuLimit && !CurrentClient->IsSuperUser())
        return TError(EError::InvalidValue, "cpu limit bigger than for parent");

    if (CurrentContainer->CpuLimit != new_limit) {
        CurrentContainer->CpuLimit = new_limit;
        CurrentContainer->SetProp(EProperty::CPU_LIMIT);
    }

    return TError::Success();
}

TError TCpuLimit::Get(std::string &value) {
    value = StringFormat("%lgc", CurrentContainer->CpuLimit);

    return TError::Success();
}

class TCpuGuarantee : public TProperty {
public:
    TError Set(const std::string &guarantee);
    TError Get(std::string &value);
    TCpuGuarantee() : TProperty(P_CPU_GUARANTEE, EProperty::CPU_GUARANTEE,
                                "CPU guarantee: 0-100.0 [%] | "
                                "0.0c-<CPUS>c [cores] (dynamic)") {}
} static CpuGuarantee;

TError TCpuGuarantee::Set(const std::string &guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_CPU);
    if (error)
        return error;

    double new_guarantee;
    error = StringToCpuValue(guarantee, new_guarantee);
    if (error)
        return error;

    if (new_guarantee > CurrentContainer->Parent->CpuGuarantee)
        L() << CurrentContainer->Name << " cpu guarantee bigger than for parent" << std::endl;

    if (CurrentContainer->CpuGuarantee != new_guarantee) {
        CurrentContainer->CpuGuarantee = new_guarantee;
        CurrentContainer->SetProp(EProperty::CPU_GUARANTEE);
    }

    return TError::Success();
}

TError TCpuGuarantee::Get(std::string &value) {
    value = StringFormat("%lgc", CurrentContainer->CpuGuarantee);

    return TError::Success();
}

class TCpuSet : public TProperty {
public:
    TCpuSet() : TProperty(P_CPU_SET, EProperty::CPU_SET,
            "CPU set: [N|N-M,]... | node N (dynamic)") {}
    TError Get(std::string &value) {
        value = CurrentContainer->CpuSet;
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_CPUSET);
        if (error)
            return error;
        if (CurrentContainer->CpuSet != value) {
            CurrentContainer->CpuSet = value;
            CurrentContainer->SetProp(EProperty::CPU_SET);
        }
        return TError::Success();
    }
} static CpuSet;

class TIoLimit : public TProperty {
public:
    TIoLimit(std::string name, EProperty prop, std::string desc) :
        TProperty(name, prop, desc) {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportIoLimit() ||
         BlkioSubsystem.HasThrottler;
    }
    TError GetMap(const TUintMap &limit, std::string &value) {
        if (limit.size() == 1 && limit.count("fs")) {
            value = std::to_string(limit.at("fs"));
            return TError::Success();
        }
        return UintMapToString(limit, value);
    }
    TError GetMapIndexed(const TUintMap &limit, const std::string &index, std::string &value) {
        if (!limit.count(index))
            return TError(EError::InvalidValue, "invalid index " + index);
        value = std::to_string(limit.at(index));
        return TError::Success();
    }
    TError SetMapMap(TUintMap &limit, const TUintMap &map) {
        TError error = IsAlive();
        if (error)
            return error;
        if (map.count("fs")) {
            error = WantControllers(CGROUP_MEMORY);
            if (error)
                return error;
        }
        if (map.size() > map.count("fs")) {
            error = WantControllers(CGROUP_BLKIO);
            if (error)
                return error;
        }
        limit = map;
        CurrentContainer->SetProp(Prop);
        return TError::Success();
    }
    TError SetMap(TUintMap &limit, const std::string &value) {
        TUintMap map;
        TError error;
        if (value.size() && value.find(':') == std::string::npos)
            error = StringToSize(value, map["fs"]);
        else
            error = StringToUintMap(value, map);
        if (error)
            return error;
        return SetMapMap(limit, map);
    }
    TError SetMapIndexed(TUintMap &limit, const std::string &index, const std::string &value) {
        TUintMap map = limit;
        TError error = StringToSize(value, map[index]);
        if (error)
            return error;
        return SetMapMap(limit, map);
    }
};

class TIoBpsLimit : public TIoLimit {
public:
    TIoBpsLimit()  : TIoLimit(P_IO_LIMIT, EProperty::IO_LIMIT,
            "IO bandwidth limit: fs|</path>|<disk> [r|w]: <bytes/s>;... (dynamic)") {}
    TError Get(std::string &value) {
        return GetMap(CurrentContainer->IoBpsLimit, value);
    }
    TError Set(const std::string &value) {
        return SetMap(CurrentContainer->IoBpsLimit, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        return GetMapIndexed(CurrentContainer->IoBpsLimit, index, value);
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        return SetMapIndexed(CurrentContainer->IoBpsLimit, index, value);
    }
} static IoBpsLimit;

class TIoOpsLimit : public TIoLimit {
public:
    TIoOpsLimit()  : TIoLimit(P_IO_OPS_LIMIT, EProperty::IO_OPS_LIMIT,
            "IOPS limit: fs|</path>|<disk> [r|w]: <iops>;... (dynamic)") {}
    TError Get(std::string &value) {
        return GetMap(CurrentContainer->IoOpsLimit, value);
    }
    TError Set(const std::string &value) {
        return SetMap(CurrentContainer->IoOpsLimit, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        return GetMapIndexed(CurrentContainer->IoOpsLimit, index, value);
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        return SetMapIndexed(CurrentContainer->IoOpsLimit, index, value);
    }
} static IoOpsLimit;

class TNetGuarantee : public TProperty {
public:
    TError Set(const std::string &guarantee);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &guarantee);
    TError GetIndexed(const std::string &index, std::string &value);
    TNetGuarantee() : TProperty(P_NET_GUARANTEE, EProperty::NET_GUARANTEE,
            "Guaranteed network bandwidth: <interface>|default: <Bps>;... (dynamic)") {}
} static NetGuarantee;

TError TNetGuarantee::Set(const std::string &guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_NETCLS);
    if (error)
        return error;

    TUintMap new_guarantee;
    error = StringToUintMap(guarantee, new_guarantee);
    if (error)
        return error;

    if (CurrentContainer->NetGuarantee != new_guarantee) {
        CurrentContainer->NetGuarantee = new_guarantee;
        CurrentContainer->SetProp(EProperty::NET_GUARANTEE);
    }

    return TError::Success();
}

TError TNetGuarantee::Get(std::string &value) {
    return UintMapToString(CurrentContainer->NetGuarantee, value);
}

TError TNetGuarantee::SetIndexed(const std::string &index,
                                          const std::string &guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t val;
    error = StringToSize(guarantee, val);
    if (error)
        return TError(EError::InvalidValue, "Invalid value " + guarantee);

    if (CurrentContainer->NetGuarantee[index] != val) {
        CurrentContainer->NetGuarantee[index] = val;
        CurrentContainer->SetProp(EProperty::NET_GUARANTEE);
    }

    return TError::Success();
}

TError TNetGuarantee::GetIndexed(const std::string &index,
                                          std::string &value) {

    if (CurrentContainer->NetGuarantee.find(index) ==
        CurrentContainer->NetGuarantee.end())

        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(CurrentContainer->NetGuarantee[index]);

    return TError::Success();
}

class TNetLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &limit);
    TError GetIndexed(const std::string &index, std::string &value);
    TNetLimit() : TProperty(P_NET_LIMIT, EProperty::NET_LIMIT,
            "Maximum network bandwidth: <interface>|default: <Bps>;... (dynamic)") {}
} static NetLimit;

TError TNetLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_NETCLS);
    if (error)
        return error;

    TUintMap new_limit;
    error = StringToUintMap(limit, new_limit);
    if (error)
        return error;

    if (CurrentContainer->NetLimit != new_limit) {
        CurrentContainer->NetLimit = new_limit;
        CurrentContainer->SetProp(EProperty::NET_LIMIT);
    }

    return TError::Success();
}

TError TNetLimit::Get(std::string &value) {
    return UintMapToString(CurrentContainer->NetLimit, value);
}

TError TNetLimit::SetIndexed(const std::string &index,
                                      const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t val;
    error = StringToSize(limit, val);
    if (error)
        return TError(EError::InvalidValue, "Invalid value " + limit);

    if (CurrentContainer->NetLimit[index] != val) {
        CurrentContainer->NetLimit[index] = val;
        CurrentContainer->SetProp(EProperty::NET_LIMIT);
    }

    return TError::Success();
}

TError TNetLimit::GetIndexed(const std::string &index,
                                      std::string &value) {

    if (CurrentContainer->NetLimit.find(index) ==
        CurrentContainer->NetLimit.end())

        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(CurrentContainer->NetLimit[index]);

    return TError::Success();
}

class TNetPriority : public TProperty {
public:
    TError Set(const std::string &prio);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &prio);
    TError GetIndexed(const std::string &index, std::string &value);
    TNetPriority()  : TProperty(P_NET_PRIO, EProperty::NET_PRIO,
            "Container network priority: <interface>|default: 0-7;... (dynamic)") {}
} static NetPriority;

TError TNetPriority::Set(const std::string &prio) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_NETCLS);
    if (error)
        return error;

    TUintMap new_prio;
    error = StringToUintMap(prio, new_prio);
    if (error)
        return error;

    for (auto &kv : new_prio) {
        if (kv.second > 7)
            return TError(EError::InvalidValue, "invalid value");
    }

    if (CurrentContainer->NetPriority != new_prio) {
        CurrentContainer->NetPriority = new_prio;
        CurrentContainer->SetProp(EProperty::NET_PRIO);
    }

    return TError::Success();
}

TError TNetPriority::Get(std::string &value) {
    return UintMapToString(CurrentContainer->NetPriority, value);
}

TError TNetPriority::SetIndexed(const std::string &index,
                                      const std::string &prio) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t val;
    error = StringToSize(prio, val);
    if (error)
        return TError(EError::InvalidValue, "Invalid value " + prio);

    if (val > 7)
        return TError(EError::InvalidValue, "invalid value");

    if (CurrentContainer->NetPriority[index] != val) {
        CurrentContainer->NetPriority[index] = val;
        CurrentContainer->SetProp(EProperty::NET_PRIO);
    }

    return TError::Success();
}

TError TNetPriority::GetIndexed(const std::string &index,
                                      std::string &value) {

    if (CurrentContainer->NetPriority.find(index) ==
        CurrentContainer->NetPriority.end())

        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(CurrentContainer->NetPriority[index]);

    return TError::Success();
}

class TRespawn : public TProperty {
public:
    TError Set(const std::string &respawn);
    TError Get(std::string &value);
    TRespawn() : TProperty(P_RESPAWN, EProperty::RESPAWN,
                           "Automatically respawn dead container (dynamic)") {}
} static Respawn;

TError TRespawn::Set(const std::string &respawn) {
    TError error = IsAlive();
    if (error)
        return error;

    if (respawn == "true")
        CurrentContainer->ToRespawn = true;
    else if (respawn == "false")
        CurrentContainer->ToRespawn = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::RESPAWN);

    return TError::Success();
}

TError TRespawn::Get(std::string &value) {
    value = CurrentContainer->ToRespawn ? "true" : "false";

    return TError::Success();
}

class TMaxRespawns : public TProperty {
public:
    TError Set(const std::string &max);
    TError Get(std::string &value);
    TMaxRespawns() : TProperty(P_MAX_RESPAWNS, EProperty::MAX_RESPAWNS,
                               "Limit respawn count for specific "
                               "container (dynamic)") {}
} static MaxRespawns;

TError TMaxRespawns::Set(const std::string &max) {
    TError error = IsAlive();
    if (error)
        return error;

    int new_value;
    if (StringToInt(max, new_value))
        return TError(EError::InvalidValue, "Invalid integer value " + max);

    CurrentContainer->MaxRespawns = new_value;
    CurrentContainer->SetProp(EProperty::MAX_RESPAWNS);

    return TError::Success();
}

TError TMaxRespawns::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MaxRespawns);

    return TError::Success();
}

class TPrivate : public TProperty {
public:
    TError Set(const std::string &max);
    TError Get(std::string &value);
    TPrivate() : TProperty(P_PRIVATE, EProperty::PRIVATE,
                           "User-defined property (dynamic)") {}
} static Private;

TError TPrivate::Set(const std::string &value) {
    TError error = IsAlive();
    if (error)
        return error;

    uint32_t max = config().container().private_max();
    if (value.length() > max)
        return TError(EError::InvalidValue, "Value is too long");

    CurrentContainer->Private = value;
    CurrentContainer->SetProp(EProperty::PRIVATE);

    return TError::Success();
}

TError TPrivate::Get(std::string &value) {
    value = CurrentContainer->Private;

    return TError::Success();
}

class TAgingTime : public TProperty {
public:
    TError Set(const std::string &time);
    TError Get(std::string &value);
    TAgingTime() : TProperty(P_AGING_TIME, EProperty::AGING_TIME,
                             "After given number of seconds "
                             "container in dead state is "
                             "automatically removed (dynamic)") {}
} static AgingTime;

TError TAgingTime::Set(const std::string &time) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_time;
    error = StringToUint64(time, new_time);
    if (error)
        return error;

    CurrentContainer->AgingTime = new_time * 1000;
    CurrentContainer->SetProp(EProperty::AGING_TIME);

    return TError::Success();
}

TError TAgingTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->AgingTime / 1000);

    return TError::Success();
}

class TEnablePorto : public TProperty {
public:
    TEnablePorto() : TProperty(P_ENABLE_PORTO, EProperty::ENABLE_PORTO,
            "Proto access level: false | read-only | child-only | true (dynamic)") {}
    TError Get(std::string &value) {
        switch (CurrentContainer->AccessLevel) {
            case EAccessLevel::None:
                value = "false";
                break;
            case EAccessLevel::ReadOnly:
                value = "read-only";
                break;
            case EAccessLevel::ChildOnly:
                value = "child-only";
                break;
            default:
                value = "true";
                break;
        }
        return TError::Success();
    }
    TError Set(const std::string &value) {
        EAccessLevel level;

        if (value == "false")
            level = EAccessLevel::None;
        else if (value == "read-only")
            level = EAccessLevel::ReadOnly;
        else if (value == "child-only")
            level = EAccessLevel::ChildOnly;
        else if (value == "true")
            level = EAccessLevel::Normal;
        else
            return TError(EError::InvalidValue, "Unknown access level: " + value);

        if (level > EAccessLevel::ChildOnly && !CurrentClient->IsSuperUser()) {
            for (auto p = CurrentContainer->Parent; p; p = p->Parent)
                if (p->AccessLevel < EAccessLevel::ChildOnly)
                    return TError(EError::Permission,
                            "Parent container has access lower than child");
        }

        CurrentContainer->AccessLevel = level;
        CurrentContainer->SetProp(EProperty::ENABLE_PORTO);
        return TError::Success();
    }
} static EnablePorto;

class TWeak : public TProperty {
public:
    TError Set(const std::string &weak);
    TError Get(std::string &value);
    TWeak() : TProperty(P_WEAK, EProperty::WEAK,
                        "Destroy container when client disconnects (dynamic)") {}
} static Weak;

TError TWeak::Set(const std::string &weak) {
    TError error = IsAlive();
    if (error)
        return error;

    if (weak == "true")
        CurrentContainer->IsWeak = true;
    else if (weak == "false")
        CurrentContainer->IsWeak = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::WEAK);

    return TError::Success();
}

TError TWeak::Get(std::string &value) {
    value = CurrentContainer->IsWeak ? "true" : "false";

    return TError::Success();
}

/* Read-only properties derived from data filelds follow below... */

class TAbsoluteName : public TProperty {
public:
    TError Get(std::string &value);
    TAbsoluteName() : TProperty(D_ABSOLUTE_NAME, EProperty::NONE,
                                "container name including "
                                "porto namespaces (ro)") {
        IsReadOnly = true;
    }
} static AbsoluteName;

TError TAbsoluteName::Get(std::string &value) {
    if (CurrentContainer->IsRoot())
        value = ROOT_CONTAINER;
    else
        value = ROOT_PORTO_NAMESPACE + CurrentContainer->Name;
    return TError::Success();
}

class TAbsoluteNamespace : public TProperty {
public:
    TError Get(std::string &value);
    TAbsoluteNamespace() : TProperty(D_ABSOLUTE_NAMESPACE, EProperty::NONE,
                                     "container namespace "
                                     "including parent "
                                     "namespaces (ro)") {
        IsReadOnly = true;
    }
} static AbsoluteNamespace;

TError TAbsoluteNamespace::Get(std::string &value) {
    value = ROOT_PORTO_NAMESPACE + CurrentContainer->GetPortoNamespace();
    return TError::Success();
}

class TState : public TProperty {
public:
    TState() : TProperty(D_STATE, EProperty::STATE, "container state (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = TContainer::StateName(CurrentContainer->State);
        return TError::Success();
    }
} static State;

class TOomKilled : public TProperty {
public:
    TOomKilled() : TProperty(D_OOM_KILLED, EProperty::OOM_KILLED,
                             "container has been killed by OOM (ro)") {
        IsReadOnly = true;
    }
    TError SetFromRestore(const std::string &value) {
        return StringToBool(value, CurrentContainer->OomKilled);
    }
    TError GetToSave(std::string &value) {
        value = BoolToString(CurrentContainer->OomKilled);
        return TError::Success();
    }
    TError Get(std::string &value) {
        TError error = IsDead();
        if (!error)
            value = BoolToString(CurrentContainer->OomKilled);
        return error;
    }
} static OomKilled;

class TOomIsFatal : public TProperty {
public:
    TOomIsFatal() : TProperty(P_OOM_IS_FATAL, EProperty::OOM_IS_FATAL,
                              "Kill all affected containers on OOM (dynamic)") {
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (!error)
            error = StringToBool(value, CurrentContainer->OomIsFatal);
        if (!error)
            CurrentContainer->SetProp(EProperty::OOM_IS_FATAL);
        return error;
    }
    TError Get(std::string &value) {
        value = BoolToString(CurrentContainer->OomIsFatal);
        return TError::Success();
    }
} static OomIsFatal;

class TParent : public TProperty {
public:
    TError Get(std::string &value);
    TParent() : TProperty(D_PARENT, EProperty::NONE,
                          "parent container name (ro) (deprecated)") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static Parent;

TError TParent::Get(std::string &value) {
    value = TContainer::ParentName(CurrentContainer->Name);
    return TError::Success();
}

class TRespawnCount : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRespawnCount() : TProperty(D_RESPAWN_COUNT, EProperty::RESPAWN_COUNT,
                                "current respawn count (ro)") {
        IsReadOnly = true;
    }
} static RespawnCount;

TError TRespawnCount::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CurrentContainer->RespawnCount);
}

TError TRespawnCount::Get(std::string &value) {
    value = std::to_string(CurrentContainer->RespawnCount);

    return TError::Success();
}

class TRootPid : public TProperty {
public:
    TRootPid() : TProperty(D_ROOT_PID, EProperty::NONE, "root task pid (ro)") {}
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        pid_t pid;
        error = CurrentContainer->GetPidFor(CurrentClient->Pid, pid);
        if (!error)
            value = std::to_string(pid);
        return error;
    }
} static RootPid;

class TExitStatusProperty : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError GetToSave(std::string &value);
    TError Get(std::string &value);
    TExitStatusProperty() : TProperty(D_EXIT_STATUS, EProperty::EXIT_STATUS,
                                      "container exit status (ro)") {
        IsReadOnly = true;
    }
} static ExitStatusProperty;

TError TExitStatusProperty::SetFromRestore(const std::string &value) {
    return StringToInt(value, CurrentContainer->ExitStatus);
}

TError TExitStatusProperty::GetToSave(std::string &value) {
    value = std::to_string(CurrentContainer->ExitStatus);

    return TError::Success();
}

TError TExitStatusProperty::Get(std::string &value) {
    TError error = IsDead();
    if (error)
        return error;

    return GetToSave(value);
}

class TExitCodeProperty : public TProperty {
public:
    TExitCodeProperty() : TProperty(D_EXIT_CODE, EProperty::NONE,
            "container exit code, negative: exit signal, OOM: -99 (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsDead();
        if (error)
            return error;
        if (CurrentContainer->OomKilled)
            value = "-99";
        else if (WIFSIGNALED(CurrentContainer->ExitStatus))
            value = std::to_string(-WTERMSIG(CurrentContainer->ExitStatus));
        else
            value = std::to_string(WEXITSTATUS(CurrentContainer->ExitStatus));
        return TError::Success();
    }
} static ExitCodeProperty;

class TMemUsage : public TProperty {
public:
    TError Get(std::string &value);
    TMemUsage() : TProperty(D_MEMORY_USAGE, EProperty::NONE,
                            "current memory usage [bytes] (ro)") {
        IsReadOnly = true;
    }
} static MemUsage;

TError TMemUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    uint64_t val;
    error = MemorySubsystem.Usage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TAnonUsage : public TProperty {
public:
    TError Get(std::string &value);
    TAnonUsage() : TProperty(D_ANON_USAGE, EProperty::NONE,
                             "current anonymous memory usage [bytes] (ro)") {
        IsReadOnly = true;
    }
} static AnonUsage;

TError TAnonUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    uint64_t val;
    error = MemorySubsystem.GetAnonUsage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class THugetlbUsage : public TProperty {
public:
    THugetlbUsage() : TProperty(D_HUGETLB_USAGE, EProperty::NONE,
                             "current hugetlb memory usage [bytes] (ro)") {
        IsReadOnly = true;
    }
    void Init(void) {
        IsSupported = HugetlbSubsystem.Supported;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        auto cg = CurrentContainer->GetCgroup(HugetlbSubsystem);
        uint64_t val;
        error = HugetlbSubsystem.GetHugeUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
    }
} static HugetlbUsage;

class TMinorFaults : public TProperty {
public:
    TError Get(std::string &value);
    TMinorFaults() : TProperty(D_MINOR_FAULTS, EProperty::NONE, "minor page faults (ro)") {
        IsReadOnly = true;
    }
} static MinorFaults;

TError TMinorFaults::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    TUintMap stat;

    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_pgfault"] - stat["total_pgmajfault"]);

    return TError::Success();
}

class TMajorFaults : public TProperty {
public:
    TError Get(std::string &value);
    TMajorFaults() : TProperty(D_MAJOR_FAULTS, EProperty::NONE, "major page faults (ro)") {
        IsReadOnly = true;
    }
} static MajorFaults;

TError TMajorFaults::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    TUintMap stat;

    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_pgmajfault"]);

    return TError::Success();
}

class TMaxRss : public TProperty {
public:
    TError Get(std::string &value);
    TMaxRss() : TProperty(D_MAX_RSS, EProperty::NONE,
                          "peak anonymous memory usage [bytes] (ro)") {
        IsReadOnly = true;
    }
    void Init(void) {
        TCgroup rootCg = MemorySubsystem.RootCgroup();
        TUintMap stat;

        TError error = MemorySubsystem.Statistics(rootCg, stat);
        IsSupported = !error && (stat.find("total_max_rss") != stat.end());
    }
} static MaxRss;

TError TMaxRss::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    TUintMap stat;
    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_max_rss"]);

    return TError::Success();
}

class TCpuUsage : public TProperty {
public:
    TError Get(std::string &value);
    TCpuUsage() : TProperty(D_CPU_USAGE, EProperty::NONE, "consumed CPU time [nanoseconds] (ro)") {
        IsReadOnly = true;
    }
} static CpuUsage;

TError TCpuUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(CpuacctSubsystem);
    uint64_t val;
    error = CpuacctSubsystem.Usage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TCpuSystem : public TProperty {
public:
    TError Get(std::string &value);
    TCpuSystem() : TProperty(D_CPU_SYSTEM, EProperty::NONE,
                             "consumed system CPU time [nanoseconds] (ro)") {
        IsReadOnly = true;
    }
} static CpuSystem;

TError TCpuSystem::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(CpuacctSubsystem);
    uint64_t val;
    error = CpuacctSubsystem.SystemUsage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TNetClassId : public TProperty {
public:
    TNetClassId() : TProperty(D_NET_CLASS_ID, EProperty::NONE,
            "network tc class: major:minor (hex) (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (!CurrentContainer->Net)
            return TError(EError::InvalidState, "not available");
        uint32_t id = CurrentContainer->ContainerTC;
        auto str = StringFormat("%x:%x", id >> 16, id & 0xFFFF);
        auto lock = CurrentContainer->Net->ScopedLock();
        TStringMap map;
        for (auto &dev: CurrentContainer->Net->Devices)
            if (dev.Managed)
                map[dev.Name] = str;
        value = StringMapToString(map);
        return TError::Success();
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (!CurrentContainer->Net)
            return TError(EError::InvalidState, "not available");
        uint32_t id = CurrentContainer->ContainerTC;
        auto lock = CurrentContainer->Net->ScopedLock();
        for (auto &dev: CurrentContainer->Net->Devices) {
            if (dev.Managed && dev.Name == index) {
                value = StringFormat("%x:%x", id >> 16, id & 0xFFFF);
                return TError::Success();
            }
        }
        return TError(EError::InvalidProperty, "network device not found");
    }
} NetClassId;

class TNetStat : public TProperty {
public:
    ENetStat Kind;

    TNetStat(std::string name, ENetStat kind, std::string desc) :
            TProperty(name, EProperty::NONE, desc) {
        Kind = kind;
        IsReadOnly = true;
    }

    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        TUintMap stat;
        error = CurrentContainer->GetNetStat(Kind, stat);
        if (error)
            return error;
        return UintMapToString(stat, value);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        TUintMap stat;
        error = CurrentContainer->GetNetStat(Kind, stat);
        if (error)
            return error;
        if (stat.find(index) == stat.end())
            return TError(EError::InvalidValue, "network device " + index + " no found");
        value = std::to_string(stat[index]);
        return TError::Success();
    }
};

TNetStat NetBytes(D_NET_BYTES, ENetStat::Bytes, "tx bytes: <interface>: <bytes>;... (ro)");
TNetStat NetPackets(D_NET_PACKETS, ENetStat::Packets, "tx packets: <interface>: <packets>;... (ro)");
TNetStat NetDrops(D_NET_DROPS, ENetStat::Drops, "tx drops: <interface>: <packets>;... (ro)");
TNetStat NetOverlimits(D_NET_OVERLIMITS, ENetStat::Overlimits, "tx overlimits: <interface>: <packets>;... (ro)");

TNetStat NetRxBytes(D_NET_RX_BYTES, ENetStat::RxBytes, "device rx bytes: <interface>: <bytes>;... (ro)");
TNetStat NetRxPackets(D_NET_RX_PACKETS, ENetStat::RxPackets, "device rx packets: <interface>: <packets>;... (ro)");
TNetStat NetRxDrops(D_NET_RX_DROPS, ENetStat::RxDrops, "device rx drops: <interface>: <packets>;... (ro)");

TNetStat NetTxBytes(D_NET_TX_BYTES, ENetStat::TxBytes, "device tx bytes: <interface>: <bytes>;... (ro)");
TNetStat NetTxPackets(D_NET_TX_PACKETS, ENetStat::TxPackets, "device tx packets: <interface>: <packets>;... (ro)");
TNetStat NetTxDrops(D_NET_TX_DROPS, ENetStat::TxDrops, "device tx drops: <interface>: <packets>;... (ro)");

class TIoStat : public TProperty {
public:
    TIoStat(std::string name, EProperty prop, std::string desc) : TProperty(name, prop, desc) {
        IsReadOnly = true;
    }
    virtual TError GetMap(TUintMap &map) = 0;
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        TUintMap map;
        error = GetMap(map);
        if (error)
            return error;
        return UintMapToString(map, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TUintMap map;
        TError error;

        error = IsRunning();
        if (error)
            return error;

        error = GetMap(map);
        if (error)
            return error;

        if (map.find(index) != map.end()) {
            value = std::to_string(map[index]);
        } else {
            std::string disk, name;

            error = BlkioSubsystem.ResolveDisk(index, disk);
            if (error)
                return error;
            error = BlkioSubsystem.DiskName(disk, name);
            if (error)
                return error;
            value = std::to_string(map[name]);
        }

        return TError::Success();
    }
};

class TIoReadStat : public TIoStat {
public:
    TIoReadStat() : TIoStat(D_IO_READ, EProperty::NONE,
            "read from disk: <disk>: <bytes>;... (ro)") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CurrentContainer->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, map, 0, false);

        if (MemorySubsystem.SupportIoLimit()) {
            auto memCg = CurrentContainer->GetCgroup(MemorySubsystem);
            TUintMap memStat;
            if (!MemorySubsystem.Statistics(memCg, memStat))
                map["fs"] = memStat["fs_io_bytes"] - memStat["fs_io_write_bytes"];
        }

        return TError::Success();
    }
} static IoReadStat;

class TIoWriteStat : public TIoStat {
public:
    TIoWriteStat() : TIoStat(D_IO_WRITE, EProperty::NONE,
            "written to disk: <disk>: <bytes>;... (ro)") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CurrentContainer->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, map, 1, false);

        if (MemorySubsystem.SupportIoLimit()) {
            auto memCg = CurrentContainer->GetCgroup(MemorySubsystem);
            TUintMap memStat;
            if (!MemorySubsystem.Statistics(memCg, memStat))
                map["fs"] = memStat["fs_io_write_bytes"];
        }

        return TError::Success();
    }
} static IoWriteStat;

class TIoOpsStat : public TIoStat {
public:
    TIoOpsStat() : TIoStat(D_IO_OPS, EProperty::NONE,
            "io operations: <disk>: <ops>;... (ro)") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CurrentContainer->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, map, 2, true);

        if (MemorySubsystem.SupportIoLimit()) {
            auto memCg = CurrentContainer->GetCgroup(MemorySubsystem);
            TUintMap memStat;
            if (!MemorySubsystem.Statistics(memCg, memStat))
                map["fs"] = memStat["fs_io_operations"];
        }

        return TError::Success();
    }
} static IoOpsStat;

class TTime : public TProperty {
public:
    TError Get(std::string &value);
    TTime() : TProperty(D_TIME, EProperty::NONE, "running time [seconds] (ro)") {
        IsReadOnly = true;
    }
} static Time;

TError TTime::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    if (CurrentContainer->IsRoot()) {
        struct sysinfo si;
        int ret = sysinfo(&si);
        if (ret)
            value = "-1";
        else
            value = std::to_string(si.uptime);

        return TError::Success();
    }

    if (!CurrentContainer->HasProp(EProperty::DEATH_TIME) &&
        (CurrentContainer->State == EContainerState::Dead)) {

        CurrentContainer->DeathTime = GetCurrentTimeMs();
        CurrentContainer->SetProp(EProperty::DEATH_TIME);
    }

    if (CurrentContainer->State == EContainerState::Dead)
        value = std::to_string((CurrentContainer->DeathTime -
                               CurrentContainer->StartTime) / 1000);
    else
        value = std::to_string((GetCurrentTimeMs() -
                               CurrentContainer->StartTime) / 1000);

    return TError::Success();
}

class TCreationTime : public TProperty {
public:
    TCreationTime() : TProperty(D_CREATION_TIME, EProperty::NONE, "creation time (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = FormatTime(CurrentContainer->RealCreationTime);
        return TError::Success();
    }
} static CreationTime;

class TStartTime : public TProperty {
public:
    TStartTime() : TProperty(D_START_TIME, EProperty::NONE, "start time (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CurrentContainer->RealStartTime)
            value = FormatTime(CurrentContainer->RealStartTime);
        return TError::Success();
    }
} static StartTime;

class TPortoStat : public TProperty {
public:
    void Populate(TUintMap &m);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TPortoStat() : TProperty(D_PORTO_STAT, EProperty::NONE, "porto statistics (ro)") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static PortoStat;

void TPortoStat::Populate(TUintMap &m) {
    m["spawned"] = Statistics->Spawned;
    m["errors"] = Statistics->Errors;
    m["warnings"] = Statistics->Warns;
    m["master_uptime"] = (GetCurrentTimeMs() - Statistics->MasterStarted) / 1000;
    m["slave_uptime"] = (GetCurrentTimeMs() - Statistics->SlaveStarted) / 1000;
    m["queued_statuses"] = Statistics->QueuedStatuses;
    m["queued_events"] = Statistics->QueuedEvents;
    m["remove_dead"] = Statistics->RemoveDead;
    m["slave_timeout_ms"] = Statistics->SlaveTimeoutMs;
    m["restore_failed"] = Statistics->RestoreFailed;
    uint64_t usage = 0;
    auto cg = MemorySubsystem.Cgroup(PORTO_DAEMON_CGROUP);
    TError error = MemorySubsystem.Usage(cg, usage);
    if (error)
        L_ERR() << "Can't get memory usage of portod" << std::endl;
    m["memory_usage_mb"] = usage / 1024 / 1024;

    m["epoll_sources"] = Statistics->EpollSources;

    m["log_rotate_bytes"] = Statistics->LogRotateBytes;
    m["log_rotate_errors"] = Statistics->LogRotateErrors;

    m["containers"] = Statistics->ContainersCount - NR_SERVICE_CONTAINERS;

    m["containers_created"] = Statistics->ContainersCreated;
    m["containers_started"] = Statistics->ContainersStarted;
    m["containers_failed_start"] = Statistics->ContainersFailedStart;
    m["containers_oom"] = Statistics->ContainersOOM;

    m["running"] = RootContainer->RunningChildren;
    m["running_children"] = CurrentContainer->RunningChildren;

    m["volumes"] = Statistics->VolumesCount;
    m["clients"] = Statistics->ClientsCount;

    m["container_clients"] = CurrentContainer->ClientsCount;
    m["container_oom"] = CurrentContainer->OomEvents;

    m["requests_queued"] = Statistics->RequestsQueued;
    m["requests_completed"] = Statistics->RequestsCompleted;

    m["requests_longer_1s"] = Statistics->RequestsLonger1s;
    m["requests_longer_3s"] = Statistics->RequestsLonger3s;
    m["requests_longer_30s"] = Statistics->RequestsLonger30s;
    m["requests_longer_5m"] = Statistics->RequestsLonger5m;
}

TError TPortoStat::Get(std::string &value) {
    TUintMap m;
    Populate(m);

    return UintMapToString(m, value);
}

TError TPortoStat::GetIndexed(const std::string &index,
                                       std::string &value) {
    TUintMap m;
    Populate(m);

    if (m.find(index) == m.end())
        return TError(EError::InvalidValue, "Invalid subscript for property");

    value = std::to_string(m[index]);

    return TError::Success();
}

class TNetTos : public TProperty {
public:
    TError Set(const std::string &tos) {
        return TError(EError::NotSupported, Name + " is not supported");
    }
    TError Get(std::string &value) {
        return TError(EError::NotSupported, "Not supported: " + Name);
    }
    TNetTos() : TProperty(P_NET_TOS, EProperty::NET_TOS, "IP TOS") {
        IsHidden = true;
        IsReadOnly = true;
        IsSupported = false;
    }
} static NetTos;

class TMemTotalLimit : public TProperty {
public:
    TMemTotalLimit() : TProperty(D_MEM_TOTAL_LIMIT, EProperty::NONE,
                                 "Total memory limit for container "
                                 "in hierarchy") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
       value = std::to_string(CurrentContainer->GetTotalMemLimit());
       return TError::Success();
    }
} static MemTotalLimit;

class TProcessCount : public TProperty {
public:
    TProcessCount() : TProperty(D_PROCESS_COUNT, EProperty::NONE, "Total process count (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        uint64_t count;
        if (CurrentContainer->IsRoot()) {
            count = 0; /* too much work */
        } else {
            auto cg = CurrentContainer->GetCgroup(FreezerSubsystem);
            error = cg.GetCount(false, count);
        }
        if (!error)
            value = std::to_string(count);
        return error;
    }
} static ProcessCount;

class TThreadCount : public TProperty {
public:
    TThreadCount() : TProperty(D_THREAD_COUNT, EProperty::NONE, "Total thread count (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        uint64_t count;
        if (CurrentContainer->IsRoot()) {
            count = GetTotalThreads();
        } else if (CurrentContainer->Controllers & CGROUP_PIDS) {
            auto cg = CurrentContainer->GetCgroup(PidsSubsystem);
            error = PidsSubsystem.GetUsage(cg, count);
        } else {
            auto cg = CurrentContainer->GetCgroup(FreezerSubsystem);
            error = cg.GetCount(true, count);
        }
        if (!error)
            value = std::to_string(count);
        return error;
    }
} static ThreadCount;

class TThreadLimit : public TProperty {
public:
    TThreadLimit() : TProperty(P_THREAD_LIMIT, EProperty::THREAD_LIMIT, "Limit pid usage (dynamic)") {}
    void Init() {
        IsSupported = PidsSubsystem.Supported;
    }
    TError Get(std::string &value) {
        if (CurrentContainer->HasProp(EProperty::THREAD_LIMIT))
            value = std::to_string(CurrentContainer->ThreadLimit);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToSize(value, val);
        if (error)
            return error;
        error = WantControllers(CGROUP_PIDS);
        if (error)
            return error;
        CurrentContainer->ThreadLimit = val;
        CurrentContainer->SetProp(EProperty::THREAD_LIMIT);
        return TError::Success();
    }
} static ThreadLimit;

void InitContainerProperties(void) {
    for (auto prop: ContainerProperties)
        prop.second->Init();
}
