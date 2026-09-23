#ifndef SQLCONDUIT_DRIVER_DRIVER_REGISTRY_H
#define SQLCONDUIT_DRIVER_DRIVER_REGISTRY_H

#include "sqlconduit/driver/idriver.h"

#include <string>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace sqlconduit::driver {
    using DriverFactoryFn = std::function<std::unique_ptr<IDriver>()>;

    struct DriverRegistration {
        std::string type;
        DriverFactoryFn factory;
    };

    class DriverRegistry {
    public:
        // Low-level process registry for custom drivers. Application-facing built-in
        // drivers should normally be registered on Client instead.
        static DriverRegistry &instance();

        void registerDriver(const std::string &type, DriverFactoryFn fn);

        void registerDriver(DriverRegistration registration);

        [[nodiscard]] bool has(const std::string &type) const;

        [[nodiscard]] std::unique_ptr<IDriver> create(const std::string &type) const;

        [[nodiscard]] std::vector<std::string> registeredTypes() const;

    private:
        mutable std::mutex mutex_;
        std::map<std::string, DriverFactoryFn> factories_;
    };
}

#endif
