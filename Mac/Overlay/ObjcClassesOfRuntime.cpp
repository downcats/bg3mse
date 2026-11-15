#include <Overlay/ObjcClasses.h>

#include <cstdlib>
#include <cstring>
#include <objc/runtime.h>

namespace bg3se::mac::overlay {

bool ObjcClassesOfRuntime::HasClass(char const* name) const
{
    return objc_lookUpClass(name) != nullptr;
}

ObjcMethod ObjcClassesOfRuntime::InstanceMethod(char const* className, char const* selector) const
{
    Class cls = objc_lookUpClass(className);
    if (!cls) return {};
    SEL wanted = sel_registerName(selector);
    Method found = class_getInstanceMethod(cls, wanted);
    if (!found) return {};

    ObjcMethod method;
    method.Found = true;
    method.Implementation = (uint64_t)(uintptr_t)method_getImplementation(found);
    if (auto types = method_getTypeEncoding(found)) method.Types = types;
    // Inherited unless the class's own list holds it.
    unsigned count = 0;
    Method* own = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count; i++) method.Own = method.Own || own[i] == found;
    free(own);
    return method;
}

ObjcVariable ObjcClassesOfRuntime::InstanceVariable(char const* className, char const* name) const
{
    Class cls = objc_lookUpClass(className);
    Ivar found = cls ? class_getInstanceVariable(cls, name) : nullptr;
    if (!found) return {};
    ObjcVariable variable;
    variable.Found = true;
    if (auto type = ivar_getTypeEncoding(found)) variable.Type = type;
    variable.Offset = ivar_getOffset(found);
    return variable;
}

std::optional<bool> ObjcClassesOfRuntime::DerivesFrom(char const* className, char const* ancestor) const
{
    for (Class cls = objc_lookUpClass(className); cls; cls = class_getSuperclass(cls)) {
        if (!strcmp(class_getName(cls), ancestor)) return true;
    }
    return false;
}

}
