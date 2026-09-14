#ifndef DBMW_DRIVER_DRIVER_REGISTRY_H
#define DBMW_DRIVER_DRIVER_REGISTRY_H

#include "dbmw/driver/idriver.h"

#include <string>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace dbmw::driver {
    using DriverFactoryFn = std::function<std::unique_ptr<IDriver>()>;

    class DriverRegistry {
    public:
        static DriverRegistry &instance();

        void registerDriver(const std::string &type, DriverFactoryFn fn);

        bool has(const std::string &type) const;

        std::unique_ptr<IDriver> create(const std::string &type) const;

        std::vector<std::string> registeredTypes() const;

    private:
        DriverRegistry() = default;

        std::map<std::string, DriverFactoryFn> factories_;
    };
}

#endif
