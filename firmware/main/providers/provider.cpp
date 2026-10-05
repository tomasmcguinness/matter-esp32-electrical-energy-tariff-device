#include "provider.h"

#include "manual_provider.h"

// The Octopus provider is parked (see main/CMakeLists.txt).
static ManualProvider s_manual;

const std::vector<Provider *> &provider_all()
{
    static const std::vector<Provider *> providers = { &s_manual };
    return providers;
}

Provider *provider_find(const std::string &id)
{
    for (Provider *p : provider_all())
    {
        if (id == p->Id())
        {
            return p;
        }
    }
    return nullptr;
}
