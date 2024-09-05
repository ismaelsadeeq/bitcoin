// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_SETTINGS_H
#define BITCOIN_INTERFACES_SETTINGS_H

#include <common/settings.h>

#include <memory>
#include <string>

namespace node {
struct  NodeContext;
} // namespacce node
namespace interfaces {

//! The action to be taken after updating a settings value.
//! WRITE indicates that the updated value must be written to disk,
//! while SKIP_WRITE indicates that the change will be kept in memory-only
//! without persisting it.
enum class SettingsAction {
    WRITE,
    SKIP_WRITE
};

class Settings {
public:
    virtual ~Settings() = default;

    //! Get settings value.
    virtual common::SettingsValue getSetting(const std::string& arg) = 0;

    //! Get list of settings values.
    virtual std::vector<common::SettingsValue> getSettingsList(const std::string& arg) = 0;

    //! Return <datadir>/settings.json setting value.
    virtual common::SettingsValue getRwSetting(const std::string& name) = 0;

    //! Updates a setting in <datadir>/settings.json.
    //! Depending on the action returned by the update function, this will either
    //! update the setting in memory or write the updated settings to disk.
    using SettingsUpdateFn = 
        std::function<std::optional<interfaces::SettingsAction>(common::SettingsValue&)>;
    virtual bool updateRwSetting(const std::string& name, const SettingsUpdateFn& update_function) = 0;

    //! Replace a setting in <datadir>/settings.json with a new value.
    virtual bool overwriteRwSetting(const std::string& name, common::SettingsValue& value, bool write = true) = 0;

    //! Delete a given setting in <datadir>/settings.json.
    virtual bool deleteRwSettings(const std::string& name, bool write = true) = 0;

    //! Clear all settings in <datadir>/settings.json and store a backup of
    //! previous settings in <datadir>/settings.json.bak.
    virtual void resetSettings() = 0;

    //! Force a setting value to be applied, overriding any other configuration
    //! source, but not being persisted.
    virtual void forceSetting(const std::string& name, const common::SettingsValue& value) = 0;

     //! Return whether a particular setting in <datadir>/settings.json is or
    //! would be ignored because it is also specified in the command line.
    virtual bool isSettingIgnored(const std::string& name) = 0;

    //! Return setting value from <datadir>/settings.json or bitcoin.conf.
    virtual common::SettingsValue getPersistentSetting(const std::string& name) = 0;

    //! Update a setting in <datadir>/settings.json.
    virtual void updateRwSetting(const std::string& name, const common::SettingsValue& value) = 0;
};

//! Return an implementation of Settings interface.
std::unique_ptr<Settings> MakeSettings(node::NodeContext& node);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_SETTINGS_H