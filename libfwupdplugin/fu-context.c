/*
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuContext"

#include "config.h"

#include "fu-bios-settings-private.h"
#include "fu-common-private.h"
#include "fu-config-private.h"
#include "fu-context-helper.h"
#include "fu-context-private.h"
#include "fu-dummy-efivars.h"
#include "fu-efi-device-path-list.h"
#include "fu-efi-file-path-device-path.h"
#include "fu-efi-hard-drive-device-path.h"
#include "fu-fdt-firmware.h"
#include "fu-hwids-private.h"
#include "fu-path.h"
#include "fu-pefile-firmware.h"
#include "fu-volume-private.h"

/**
 * FuContext:
 *
 * A context that represents the shared system state. This object is shared
 * between the engine, the plugins and the devices.
 */

typedef struct {
	FuContextFlags flags;
	FuHwids *hwids;
	FuConfig *config;
	FuSmbios *smbios;
	FuSmbiosChassisKind chassis_kind;
	FuQuirks *quirks;
	FuEfivars *efivars;
	GPtrArray *backends;
	GHashTable *runtime_versions;
	GHashTable *compile_versions;
	GHashTable *udev_subsystems; /* utf8:GPtrArray */
	GPtrArray *esp_volumes;
	GHashTable *firmware_gtypes; /* utf8:GType */
	GHashTable *hwid_flags;	     /* str: */
	FuPowerState power_state;
	FuLidState lid_state;
	FuDisplayState display_state;
	guint battery_level;
	guint battery_threshold;
	FuBiosSettings *host_bios_settings;
	FuFirmware *fdt; /* optional */
	gchar *esp_location;
} FuContextPrivate;

enum { SIGNAL_SECURITY_CHANGED, SIGNAL_HOUSEKEEPING, SIGNAL_LAST };

enum {
	PROP_0,
	PROP_POWER_STATE,
	PROP_LID_STATE,
	PROP_DISPLAY_STATE,
	PROP_BATTERY_LEVEL,
	PROP_BATTERY_THRESHOLD,
	PROP_FLAGS,
	PROP_LAST
};

static guint signals[SIGNAL_LAST] = {0};

G_DEFINE_TYPE_WITH_PRIVATE(FuContext, fu_context, G_TYPE_OBJECT)

#define GET_PRIVATE(o) (fu_context_get_instance_private(o))

static GFile *
fu_context_get_fdt_file(GError **error)
{
	g_autofree gchar *fdtfn_local = NULL;
	g_autofree gchar *fdtfn_sys = NULL;
	g_autofree gchar *localstatedir_pkg = fu_path_from_kind(FU_PATH_KIND_LOCALSTATEDIR_PKG);
	g_autofree gchar *sysfsdir = NULL;

	/* look for override first, fall back to system value */
	fdtfn_local = g_build_filename(localstatedir_pkg, "system.dtb", NULL);
	if (g_file_test(fdtfn_local, G_FILE_TEST_EXISTS))
		return g_file_new_for_path(fdtfn_local);

	/* actual hardware value */
	sysfsdir = fu_path_from_kind(FU_PATH_KIND_SYSFSDIR_FW);
	fdtfn_sys = g_build_filename(sysfsdir, "fdt", NULL);
	if (g_file_test(fdtfn_sys, G_FILE_TEST_EXISTS))
		return g_file_new_for_path(fdtfn_sys);

	/* failed */
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    "cannot find %s or override %s",
		    fdtfn_sys,
		    fdtfn_local);
	return NULL;
}

/**
 * fu_context_get_fdt:
 * @self: a #FuContext
 * @error: (nullable): optional return location for an error
 *
 * Gets and parses the system FDT, aka. the Flat Device Tree.
 *
 * The results are cached internally to the context, and subsequent calls to this function
 * returns the pre-parsed object.
 *
 * Returns: (transfer full): a #FuFdtFirmware, or %NULL
 *
 * Since: 1.8.10
 **/
FuFirmware *
fu_context_get_fdt(FuContext *self, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* load if not already parsed */
	if (priv->fdt == NULL) {
		g_autoptr(FuFirmware) fdt_tmp = fu_fdt_firmware_new();
		g_autoptr(GFile) file = fu_context_get_fdt_file(error);
		if (file == NULL)
			return NULL;
		if (!fu_firmware_parse_file(fdt_tmp,
					    file,
					    FU_FIRMWARE_PARSE_FLAG_NO_SEARCH,
					    error)) {
			g_prefix_error(error, "failed to parse FDT: ");
			return NULL;
		}
		priv->fdt = g_steal_pointer(&fdt_tmp);
	}

	/* success */
	return g_object_ref(priv->fdt);
}

/**
 * fu_context_get_efivars:
 * @self: a #FuContext
 *
 * Gets the EFI variable store.
 *
 * Returns: (transfer none): a #FuEfivars
 *
 * Since: 2.0.0
 **/
FuEfivars *
fu_context_get_efivars(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->efivars;
}

/**
 * fu_context_efivars_check_free_space:
 * @self: a #FuContext
 * @count: size in bytes
 * @error: (nullable): optional return location for an error
 *
 * Checks for a given amount of free space in the EFI NVRAM variable store.
 *
 * Returns: %TRUE for success
 *
 * Since: 2.0.12
 **/
gboolean
fu_context_efivars_check_free_space(FuContext *self, gsize count, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	guint64 total;

	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* escape hatch */
	if (fu_context_has_flag(self, FU_CONTEXT_FLAG_IGNORE_EFIVARS_FREE_SPACE))
		return TRUE;

	total = fu_efivars_space_free(priv->efivars, error);
	if (total == G_MAXUINT64)
		return FALSE;
	if (total < count) {
		g_autofree gchar *countstr = g_format_size(count);
		g_autofree gchar *totalstr = g_format_size(total);
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_BROKEN_SYSTEM,
			    "Not enough efivarfs space, requested %s and got %s",
			    countstr,
			    totalstr);
		return FALSE;
	}
	return TRUE;
}

/**
 * fu_context_get_smbios:
 * @self: a #FuContext
 *
 * Gets the SMBIOS store.
 *
 * Returns: (transfer none): a #FuSmbios
 *
 * Since: 1.8.10
 **/
FuSmbios *
fu_context_get_smbios(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->smbios;
}

/**
 * fu_context_get_hwids:
 * @self: a #FuContext
 *
 * Gets the HWIDs store.
 *
 * Returns: (transfer none): a #FuHwids
 *
 * Since: 1.8.10
 **/
FuHwids *
fu_context_get_hwids(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->hwids;
}

/**
 * fu_context_get_config:
 * @self: a #FuContext
 *
 * Gets the system config.
 *
 * Returns: (transfer none): a #FuHwids
 *
 * Since: 1.9.1
 **/
FuConfig *
fu_context_get_config(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->config;
}

/**
 * fu_context_get_smbios_string:
 * @self: a #FuContext
 * @type: a SMBIOS structure type, e.g. %FU_SMBIOS_STRUCTURE_TYPE_BIOS
 * @length: expected length of the structure, or %FU_SMBIOS_STRUCTURE_LENGTH_ANY
 * @offset: a SMBIOS offset
 * @error: (nullable): optional return location for an error
 *
 * Gets a hardware SMBIOS string.
 *
 * The @type and @offset can be referenced from the DMTF SMBIOS specification:
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_3.1.1.pdf
 *
 * Returns: a string, or %NULL
 *
 * Since: 2.0.7
 **/
const gchar *
fu_context_get_smbios_string(FuContext *self,
			     guint8 type,
			     guint8 length,
			     guint8 offset,
			     GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use SMBIOS before calling ->load_hwinfo()");
		return NULL;
	}
	return fu_smbios_get_string(priv->smbios, type, length, offset, error);
}

/**
 * fu_context_get_smbios_data:
 * @self: a #FuContext
 * @type: a SMBIOS structure type, e.g. %FU_SMBIOS_STRUCTURE_TYPE_BIOS
 * @length: expected length of the structure, or %FU_SMBIOS_STRUCTURE_LENGTH_ANY
 * @error: (nullable): optional return location for an error
 *
 * Gets all hardware SMBIOS data for a specific type.
 *
 * Returns: (transfer container) (element-type GBytes): a #GBytes, or %NULL if not found
 *
 * Since: 2.0.7
 **/
GPtrArray *
fu_context_get_smbios_data(FuContext *self, guint8 type, guint8 length, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);

	/* must be valid and non-zero length */
	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use SMBIOS before calling ->load_hwinfo()");
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "no data");
		return NULL;
	}
	return fu_smbios_get_data(priv->smbios, type, length, error);
}

/**
 * fu_context_get_smbios_integer:
 * @self: a #FuContext
 * @type: a structure type, e.g. %FU_SMBIOS_STRUCTURE_TYPE_BIOS
 * @length: expected length of the structure, or %FU_SMBIOS_STRUCTURE_LENGTH_ANY
 * @offset: a structure offset
 * @error: (nullable): optional return location for an error
 *
 * Reads an integer value from the SMBIOS string table of a specific structure.
 *
 * The @type and @offset can be referenced from the DMTF SMBIOS specification:
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_3.1.1.pdf
 *
 * Returns: an integer, or %G_MAXUINT if invalid or not found
 *
 * Since: 2.0.7
 **/
guint
fu_context_get_smbios_integer(FuContext *self,
			      guint8 type,
			      guint8 length,
			      guint8 offset,
			      GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), G_MAXUINT);
	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use SMBIOS before calling ->load_hwinfo()");
		return G_MAXUINT;
	}
	return fu_smbios_get_integer(priv->smbios, type, length, offset, error);
}

/**
 * fu_context_reload_bios_settings:
 * @self: a #FuContext
 * @error: (nullable): optional return location for an error
 *
 * Refreshes the list of firmware attributes on the system.
 *
 * Since: 1.8.4
 **/
gboolean
fu_context_reload_bios_settings(FuContext *self, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	return fu_bios_settings_setup(priv->host_bios_settings, error);
}

/**
 * fu_context_get_bios_settings:
 * @self: a #FuContext
 *
 * Returns all the firmware attributes defined in the system.
 *
 * Returns: (transfer full): A #FuBiosSettings
 *
 * Since: 1.8.4
 **/
FuBiosSettings *
fu_context_get_bios_settings(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return g_object_ref(priv->host_bios_settings);
}

/**
 * fu_context_get_bios_setting:
 * @self: a #FuContext
 * @name: a BIOS setting title, e.g. `BootOrderLock`
 *
 * Finds out if a system supports a given BIOS setting.
 *
 * Returns: (transfer none): #FwupdBiosSetting if the attr exists.
 *
 * Since: 1.8.4
 **/
FwupdBiosSetting *
fu_context_get_bios_setting(FuContext *self, const gchar *name)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);
	return fu_bios_settings_get_attr(priv->host_bios_settings, name);
}

/**
 * fu_context_get_bios_setting_pending_reboot:
 * @self: a #FuContext
 *
 * Determine if updates to BIOS settings are pending until next boot.
 *
 * Returns: %TRUE if updates are pending.
 *
 * Since: 1.8.4
 **/
gboolean
fu_context_get_bios_setting_pending_reboot(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	gboolean ret;
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	fu_bios_settings_get_pending_reboot(priv->host_bios_settings, &ret, NULL);
	return ret;
}

/**
 * fu_context_get_chassis_kind:
 * @self: a #FuContext
 *
 * Gets the chassis kind, if known.
 *
 * Returns: a #FuSmbiosChassisKind, e.g. %FU_SMBIOS_CHASSIS_KIND_LAPTOP
 *
 * Since: 1.8.10
 **/
FuSmbiosChassisKind
fu_context_get_chassis_kind(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	return priv->chassis_kind;
}

/**
 * fu_context_set_chassis_kind:
 * @self: a #FuContext
 * @chassis_kind: a #FuSmbiosChassisKind, e.g. %FU_SMBIOS_CHASSIS_KIND_TABLET
 *
 * Sets the chassis kind.
 *
 * Since: 1.8.10
 **/
void
fu_context_set_chassis_kind(FuContext *self, FuSmbiosChassisKind chassis_kind)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	priv->chassis_kind = chassis_kind;
}

/**
 * fu_context_has_hwid_guid:
 * @self: a #FuContext
 * @guid: a GUID, e.g. `059eb22d-6dc7-59af-abd3-94bbe017f67c`
 *
 * Finds out if a hardware GUID exists.
 *
 * Returns: %TRUE if the GUID exists
 *
 * Since: 1.6.0
 **/
gboolean
fu_context_has_hwid_guid(FuContext *self, const gchar *guid)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use HWIDs before calling ->load_hwinfo()");
		return FALSE;
	}
	return fu_hwids_has_guid(priv->hwids, guid);
}

/**
 * fu_context_get_hwid_guids:
 * @self: a #FuContext
 *
 * Returns all the HWIDs defined in the system. All hardware IDs on a
 * specific system can be shown using the `fwupdmgr hwids` command.
 *
 * Returns: (transfer none) (element-type utf8): an array of GUIDs
 *
 * Since: 1.6.0
 **/
GPtrArray *
fu_context_get_hwid_guids(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use HWIDs before calling ->load_hwinfo()");
		return NULL;
	}
	return fu_hwids_get_guids(priv->hwids);
}

/**
 * fu_context_get_hwid_value:
 * @self: a #FuContext
 * @key: a DMI ID, e.g. `BiosVersion`
 *
 * Gets the cached value for one specific key that is valid ASCII and suitable
 * for display.
 *
 * Returns: the string, e.g. `1.2.3`, or %NULL if not found
 *
 * Since: 1.6.0
 **/
const gchar *
fu_context_get_hwid_value(FuContext *self, const gchar *key)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(key != NULL, NULL);
	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use HWIDs before calling ->load_hwinfo()");
		return NULL;
	}
	return fu_hwids_get_value(priv->hwids, key);
}

/**
 * fu_context_get_hwid_replace_value:
 * @self: a #FuContext
 * @keys: a key, e.g. `HardwareID-3` or %FU_HWIDS_KEY_PRODUCT_SKU
 * @error: (nullable): optional return location for an error
 *
 * Gets the replacement value for a specific key. All hardware IDs on a
 * specific system can be shown using the `fwupdmgr hwids` command.
 *
 * Returns: (transfer full): a string, or %NULL for error.
 *
 * Since: 1.6.0
 **/
gchar *
fu_context_get_hwid_replace_value(FuContext *self, const gchar *keys, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(keys != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (!fu_context_has_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO)) {
		g_critical("cannot use HWIDs before calling ->load_hwinfo()");
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "no data");
		return NULL;
	}
	return fu_hwids_get_replace_values(priv->hwids, keys, error);
}

/**
 * fu_context_add_runtime_version:
 * @self: a #FuContext
 * @component_id: an AppStream component id, e.g. `org.gnome.Software`
 * @version: a version string, e.g. `1.2.3`
 *
 * Sets a runtime version of a specific dependency.
 *
 * Since: 1.6.0
 **/
void
fu_context_add_runtime_version(FuContext *self, const gchar *component_id, const gchar *version)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(component_id != NULL);
	g_return_if_fail(version != NULL);

	if (priv->runtime_versions == NULL)
		return;
	g_hash_table_insert(priv->runtime_versions, g_strdup(component_id), g_strdup(version));
}

/**
 * fu_context_get_runtime_version:
 * @self: a #FuContext
 * @component_id: an AppStream component id, e.g. `org.gnome.Software`
 *
 * Sets a runtime version of a specific dependency.
 *
 * Returns: a version string, e.g. `1.2.3`, or %NULL
 *
 * Since: 1.9.10
 **/
const gchar *
fu_context_get_runtime_version(FuContext *self, const gchar *component_id)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(component_id != NULL, NULL);

	if (priv->runtime_versions == NULL)
		return NULL;
	return g_hash_table_lookup(priv->runtime_versions, component_id);
}

/**
 * fu_context_get_runtime_versions:
 * @self: a #FuContext
 *
 * Gets the runtime versions for the context.
 *
 * Returns: (transfer none) (element-type utf8 utf8): dictionary of versions
 *
 * Since: 1.9.10
 **/
GHashTable *
fu_context_get_runtime_versions(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->runtime_versions;
}

/**
 * fu_context_add_compile_version:
 * @self: a #FuContext
 * @component_id: an AppStream component id, e.g. `org.gnome.Software`
 * @version: a version string, e.g. `1.2.3`
 *
 * Sets a compile-time version of a specific dependency.
 *
 * Since: 1.6.0
 **/
void
fu_context_add_compile_version(FuContext *self, const gchar *component_id, const gchar *version)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(component_id != NULL);
	g_return_if_fail(version != NULL);

	if (priv->compile_versions == NULL)
		return;
	g_hash_table_insert(priv->compile_versions, g_strdup(component_id), g_strdup(version));
}

/**
 * fu_context_get_compile_versions:
 * @self: a #FuContext
 *
 * Gets the compile time versions for the context.
 *
 * Returns: (transfer none) (element-type utf8 utf8): dictionary of versions
 *
 * Since: 1.9.10
 **/
GHashTable *
fu_context_get_compile_versions(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->compile_versions;
}

static gint
fu_context_udev_plugin_names_sort_cb(gconstpointer a, gconstpointer b)
{
	const gchar *str_a = *((const gchar **)a);
	const gchar *str_b = *((const gchar **)b);
	return g_strcmp0(str_a, str_b);
}

/**
 * fu_context_add_udev_subsystem:
 * @self: a #FuContext
 * @subsystem: a subsystem name, e.g. `pciport`, or `block:partition`
 * @plugin_name: (nullable): a plugin name, e.g. `iommu`
 *
 * Registers the udev subsystem to be watched by the daemon.
 *
 * Plugins can use this method only in fu_plugin_init()
 *
 * Since: 1.6.0
 **/
void
fu_context_add_udev_subsystem(FuContext *self, const gchar *subsystem, const gchar *plugin_name)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	GPtrArray *plugin_names;
	g_auto(GStrv) subsystem_devtype = NULL;

	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(subsystem != NULL);

	/* add the base subsystem watch if passed a subsystem:devtype */
	subsystem_devtype = g_strsplit(subsystem, ":", 2);
	if (g_strv_length(subsystem_devtype) > 1)
		fu_context_add_udev_subsystem(self, subsystem_devtype[0], NULL);

	/* already exists */
	plugin_names = g_hash_table_lookup(priv->udev_subsystems, subsystem);
	if (plugin_names != NULL) {
		if (plugin_name != NULL) {
			for (guint i = 0; i < plugin_names->len; i++) {
				const gchar *tmp = g_ptr_array_index(plugin_names, i);
				if (g_strcmp0(tmp, plugin_name) == 0)
					return;
			}
			g_ptr_array_add(plugin_names, g_strdup(plugin_name));
			g_ptr_array_sort(plugin_names, fu_context_udev_plugin_names_sort_cb);
		}
		return;
	}

	/* add */
	plugin_names = g_ptr_array_new_with_free_func(g_free);
	if (plugin_name != NULL)
		g_ptr_array_add(plugin_names, g_strdup(plugin_name));
	g_hash_table_insert(priv->udev_subsystems,
			    g_strdup(subsystem),
			    g_steal_pointer(&plugin_names));
	if (plugin_name != NULL)
		g_info("added udev subsystem watch of %s for plugin %s", subsystem, plugin_name);
	else
		g_info("added udev subsystem watch of %s", subsystem);
}

/**
 * fu_context_get_plugin_names_for_udev_subsystem:
 * @self: a #FuContext
 * @subsystem: a subsystem name, e.g. `pciport`, or `block:partition`
 * @error: (nullable): optional return location for an error
 *
 * Gets the plugins which registered for a specific subsystem.
 *
 * Returns: (transfer container) (element-type utf8): List of plugin names
 *
 * Since: 1.9.3
 **/
GPtrArray *
fu_context_get_plugin_names_for_udev_subsystem(FuContext *self,
					       const gchar *subsystem,
					       GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	GPtrArray *plugin_names_tmp;
	g_auto(GStrv) subsystem_devtype = NULL;
	g_autoptr(GPtrArray) plugin_names = g_ptr_array_new_with_free_func(g_free);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(subsystem != NULL, NULL);

	/* add the base subsystem first */
	subsystem_devtype = g_strsplit(subsystem, ":", 2);
	if (g_strv_length(subsystem_devtype) > 1) {
		plugin_names_tmp = g_hash_table_lookup(priv->udev_subsystems, subsystem_devtype[0]);
		if (plugin_names_tmp != NULL)
			g_ptr_array_extend(plugin_names,
					   plugin_names_tmp,
					   (GCopyFunc)g_strdup,
					   NULL);
	}

	/* add the exact match */
	plugin_names_tmp = g_hash_table_lookup(priv->udev_subsystems, subsystem);
	if (plugin_names_tmp != NULL)
		g_ptr_array_extend(plugin_names, plugin_names_tmp, (GCopyFunc)g_strdup, NULL);

	/* no matches */
	if (plugin_names->len == 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "no plugins registered for %s",
			    subsystem);
		return NULL;
	}

	/* success */
	return g_steal_pointer(&plugin_names);
}

/**
 * fu_context_get_udev_subsystems:
 * @self: a #FuContext
 *
 * Gets the udev subsystems required by all plugins.
 *
 * Returns: (transfer container) (element-type utf8): List of subsystems
 *
 * Since: 1.6.0
 **/
GPtrArray *
fu_context_get_udev_subsystems(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_autoptr(GList) keys = g_hash_table_get_keys(priv->udev_subsystems);
	g_autoptr(GPtrArray) subsystems = g_ptr_array_new_with_free_func(g_free);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);

	for (GList *l = keys; l != NULL; l = l->next) {
		const gchar *subsystem = (const gchar *)l->data;
		g_ptr_array_add(subsystems, g_strdup(subsystem));
	}
	return g_steal_pointer(&subsystems);
}

/**
 * fu_context_add_firmware_gtype:
 * @self: a #FuContext
 * @id: (nullable): an optional string describing the type, e.g. `ihex`
 * @gtype: a #GType e.g. `FU_TYPE_FOO_FIRMWARE`
 *
 * Adds a firmware #GType which is used when creating devices. If @id is not
 * specified then it is guessed using the #GType name.
 *
 * Plugins can use this method only in fu_plugin_init()
 *
 * Since: 1.6.0
 **/
void
fu_context_add_firmware_gtype(FuContext *self, const gchar *id, GType gtype)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(id != NULL);
	g_return_if_fail(gtype != G_TYPE_INVALID);
	g_type_ensure(gtype);
	g_hash_table_insert(priv->firmware_gtypes, g_strdup(id), GSIZE_TO_POINTER(gtype));
}

/**
 * fu_context_get_firmware_gtype_by_id:
 * @self: a #FuContext
 * @id: an string describing the type, e.g. `ihex`
 *
 * Returns the #GType using the firmware @id.
 *
 * Returns: a #GType, or %G_TYPE_INVALID
 *
 * Since: 1.6.0
 **/
GType
fu_context_get_firmware_gtype_by_id(FuContext *self, const gchar *id)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), G_TYPE_INVALID);
	g_return_val_if_fail(id != NULL, G_TYPE_INVALID);
	return GPOINTER_TO_SIZE(g_hash_table_lookup(priv->firmware_gtypes, id));
}

static gint
fu_context_gtypes_sort_cb(gconstpointer a, gconstpointer b)
{
	const gchar *stra = *((const gchar **)a);
	const gchar *strb = *((const gchar **)b);
	return g_strcmp0(stra, strb);
}

/**
 * fu_context_get_firmware_gtype_ids:
 * @self: a #FuContext
 *
 * Returns all the firmware #GType IDs.
 *
 * Returns: (transfer container) (element-type utf8): firmware IDs
 *
 * Since: 1.6.0
 **/
GPtrArray *
fu_context_get_firmware_gtype_ids(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	GPtrArray *firmware_gtypes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GList) keys = g_hash_table_get_keys(priv->firmware_gtypes);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);

	for (GList *l = keys; l != NULL; l = l->next) {
		const gchar *id = l->data;
		g_ptr_array_add(firmware_gtypes, g_strdup(id));
	}
	g_ptr_array_sort(firmware_gtypes, fu_context_gtypes_sort_cb);
	return firmware_gtypes;
}

/**
 * fu_context_get_firmware_gtypes:
 * @self: a #FuContext
 *
 * Returns all the firmware #GType's.
 *
 * Returns: (transfer container) (element-type GType): Firmware types
 *
 * Since: 1.9.1
 **/
GArray *
fu_context_get_firmware_gtypes(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	GArray *firmware_gtypes = g_array_new(FALSE, FALSE, sizeof(GType));
	g_autoptr(GList) values = g_hash_table_get_values(priv->firmware_gtypes);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);

	for (GList *l = values; l != NULL; l = l->next) {
		GType gtype = GPOINTER_TO_SIZE(l->data);
		g_array_append_val(firmware_gtypes, gtype);
	}
	return firmware_gtypes;
}

/**
 * fu_context_add_quirk_key:
 * @self: a #FuContext
 * @key: a quirk string, e.g. `DfuVersion`
 *
 * Adds a possible quirk key. If added by a plugin it should be namespaced
 * using the plugin name, where possible.
 *
 * Plugins can use this method only in fu_plugin_init()
 *
 * Since: 1.6.0
 **/
void
fu_context_add_quirk_key(FuContext *self, const gchar *key)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(key != NULL);
	if (priv->quirks == NULL)
		return;
	fu_quirks_add_possible_key(priv->quirks, key);
}

/**
 * fu_context_lookup_quirk_by_id:
 * @self: a #FuContext
 * @guid: GUID to lookup
 * @key: an ID to match the entry, e.g. `Summary`
 *
 * Looks up an entry in the hardware database using a string value.
 *
 * Returns: (transfer none): values from the database, or %NULL if not found
 *
 * Since: 1.6.0
 **/
const gchar *
fu_context_lookup_quirk_by_id(FuContext *self, const gchar *guid, const gchar *key)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(guid != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);

	/* exact ID */
	return fu_quirks_lookup_by_id(priv->quirks, guid, key);
}

typedef struct {
	FuContext *self; /* noref */
	FuContextLookupIter iter_cb;
	gpointer user_data;
} FuContextQuirkLookupHelper;

static void
fu_context_lookup_quirk_by_id_iter_cb(FuQuirks *self,
				      const gchar *key,
				      const gchar *value,
				      FuContextQuirkSource source,
				      gpointer user_data)
{
	FuContextQuirkLookupHelper *helper = (FuContextQuirkLookupHelper *)user_data;
	helper->iter_cb(helper->self, key, value, source, helper->user_data);
}

/**
 * fu_context_lookup_quirk_by_id_iter:
 * @self: a #FuContext
 * @guid: GUID to lookup
 * @key: (nullable): an ID to match the entry, e.g. `Name`, or %NULL for all keys
 * @iter_cb: (scope call) (closure user_data): a function to call for each result
 * @user_data: user data passed to @iter_cb
 *
 * Looks up all entries in the hardware database using a GUID value.
 *
 * Returns: %TRUE if the ID was found, and @iter was called
 *
 * Since: 1.6.0
 **/
gboolean
fu_context_lookup_quirk_by_id_iter(FuContext *self,
				   const gchar *guid,
				   const gchar *key,
				   FuContextLookupIter iter_cb,
				   gpointer user_data)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	FuContextQuirkLookupHelper helper = {
	    .self = self,
	    .iter_cb = iter_cb,
	    .user_data = user_data,
	};
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(guid != NULL, FALSE);
	g_return_val_if_fail(iter_cb != NULL, FALSE);
	return fu_quirks_lookup_by_id_iter(priv->quirks,
					   guid,
					   key,
					   fu_context_lookup_quirk_by_id_iter_cb,
					   &helper);
}

/**
 * fu_context_security_changed:
 * @self: a #FuContext
 *
 * Informs the daemon that the HSI state may have changed.
 *
 * Since: 1.6.0
 **/
void
fu_context_security_changed(FuContext *self)
{
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_signal_emit(self, signals[SIGNAL_SECURITY_CHANGED], 0);
}

/**
 * fu_context_housekeeping:
 * @self: a #FuContext
 *
 * Performs any housekeeping maintenance when the daemon is idle.
 *
 * Since: 2.0.0
 **/
void
fu_context_housekeeping(FuContext *self)
{
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_signal_emit(self, signals[SIGNAL_HOUSEKEEPING], 0);
}

typedef gboolean (*FuContextHwidsSetupFunc)(FuContext *self, FuHwids *hwids, GError **error);

static void
fu_context_detect_full_disk_encryption(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GError) error_local = NULL;

	g_return_if_fail(FU_IS_CONTEXT(self));

	devices = fu_common_get_block_devices(&error_local);
	if (devices == NULL) {
		g_info("Failed to get block devices: %s", error_local->message);
		return;
	}

	for (guint i = 0; i < devices->len; i++) {
		GDBusProxy *proxy = g_ptr_array_index(devices, i);
		g_autoptr(GVariant) id_type = g_dbus_proxy_get_cached_property(proxy, "IdType");
		g_autoptr(GVariant) device = g_dbus_proxy_get_cached_property(proxy, "Device");
		g_autoptr(GVariant) id_label = g_dbus_proxy_get_cached_property(proxy, "IdLabel");
		if (id_type != NULL && device != NULL &&
		    g_strcmp0(g_variant_get_string(id_type, NULL), "BitLocker") == 0)
			priv->flags |= FU_CONTEXT_FLAG_FDE_BITLOCKER;

		if (id_type != NULL && id_label != NULL &&
		    g_strcmp0(g_variant_get_string(id_label, NULL), "ubuntu-data-enc") == 0 &&
		    g_strcmp0(g_variant_get_string(id_type, NULL), "crypto_LUKS") == 0) {
			priv->flags |= FU_CONTEXT_FLAG_FDE_SNAPD;
		}
	}
}

static void
fu_context_hwid_quirk_cb(FuContext *self,
			 const gchar *key,
			 const gchar *value,
			 FuContextQuirkSource source,
			 gpointer user_data)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	if (value != NULL) {
		g_auto(GStrv) values = g_strsplit(value, ",", -1);
		for (guint j = 0; values[j] != NULL; j++)
			g_hash_table_add(priv->hwid_flags, g_strdup(values[j]));
	}
}

/**
 * fu_context_load_hwinfo:
 * @self: a #FuContext
 * @progress: a #FuProgress
 * @flags: a #FuContextHwidFlags, e.g. %FU_CONTEXT_HWID_FLAG_LOAD_SMBIOS
 * @error: (nullable): optional return location for an error
 *
 * Loads all hardware information parts of the context.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.8.10
 **/
gboolean
fu_context_load_hwinfo(FuContext *self,
		       FuProgress *progress,
		       FuContextHwidFlags flags,
		       GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	GPtrArray *guids;
	g_autoptr(GError) error_hwids = NULL;
	g_autoptr(GError) error_bios_settings = NULL;
	struct {
		const gchar *name;
		FuContextHwidFlags flag;
		FuContextHwidsSetupFunc func;
	} hwids_setup_map[] = {{"config", FU_CONTEXT_HWID_FLAG_LOAD_CONFIG, fu_hwids_config_setup},
			       {"smbios", FU_CONTEXT_HWID_FLAG_LOAD_SMBIOS, fu_hwids_smbios_setup},
			       {"fdt", FU_CONTEXT_HWID_FLAG_LOAD_FDT, fu_hwids_fdt_setup},
			       {"kenv", FU_CONTEXT_HWID_FLAG_LOAD_KENV, fu_hwids_kenv_setup},
			       {"dmi", FU_CONTEXT_HWID_FLAG_LOAD_DMI, fu_hwids_dmi_setup},
			       {"darwin", FU_CONTEXT_HWID_FLAG_LOAD_DARWIN, fu_hwids_darwin_setup},
			       {NULL, FU_CONTEXT_HWID_FLAG_NONE, NULL}};

	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_LOADING, 1, "hwids-setup-funcs");
	fu_progress_add_step(progress, FWUPD_STATUS_LOADING, 1, "hwids-setup");
	fu_progress_add_step(progress, FWUPD_STATUS_LOADING, 3, "set-flags");
	fu_progress_add_step(progress, FWUPD_STATUS_LOADING, 1, "detect-fde");
	fu_progress_add_step(progress, FWUPD_STATUS_LOADING, 94, "reload-bios-settings");

	/* required always */
	if (!fu_config_load(priv->config, error))
		return FALSE;

	/* run all the HWID setup funcs */
	for (guint i = 0; hwids_setup_map[i].name != NULL; i++) {
		if ((flags & hwids_setup_map[i].flag) > 0) {
			g_autoptr(GError) error_local = NULL;
			if (!hwids_setup_map[i].func(self, priv->hwids, &error_local)) {
				g_info("failed to load %s: %s",
				       hwids_setup_map[i].name,
				       error_local->message);
				continue;
			}
		}
	}
	fu_context_add_flag(self, FU_CONTEXT_FLAG_LOADED_HWINFO);
	fu_progress_step_done(progress);

	if (!fu_hwids_setup(priv->hwids, &error_hwids))
		g_warning("Failed to load HWIDs: %s", error_hwids->message);
	fu_progress_step_done(progress);

	/* set the hwid flags */
	guids = fu_context_get_hwid_guids(self);
	for (guint i = 0; i < guids->len; i++) {
		const gchar *guid = g_ptr_array_index(guids, i);
		fu_context_lookup_quirk_by_id_iter(self,
						   guid,
						   FU_QUIRKS_FLAGS,
						   fu_context_hwid_quirk_cb,
						   NULL);
	}
	fu_progress_step_done(progress);

	fu_context_detect_full_disk_encryption(self);
	fu_progress_step_done(progress);

	fu_context_add_udev_subsystem(self, "firmware-attributes", NULL);
	if (!fu_context_reload_bios_settings(self, &error_bios_settings))
		g_debug("%s", error_bios_settings->message);
	fu_progress_step_done(progress);

	/* always */
	return TRUE;
}

/**
 * fu_context_has_hwid_flag:
 * @self: a #FuContext
 * @flag: flag, e.g. `use-legacy-bootmgr-desc`
 *
 * Returns if a HwId custom flag exists, typically added from a DMI quirk.
 *
 * Returns: %TRUE if the flag exists
 *
 * Since: 1.7.2
 **/
gboolean
fu_context_has_hwid_flag(FuContext *self, const gchar *flag)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(flag != NULL, FALSE);
	return g_hash_table_lookup(priv->hwid_flags, flag) != NULL;
}

/**
 * fu_context_load_quirks:
 * @self: a #FuContext
 * @flags: quirks load flags, e.g. %FU_QUIRKS_LOAD_FLAG_READONLY_FS
 * @error: (nullable): optional return location for an error
 *
 * Loads all quirks into the context.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.6.0
 **/
gboolean
fu_context_load_quirks(FuContext *self, FuQuirksLoadFlags flags, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_autoptr(GError) error_local = NULL;

	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* rebuild silo if required */
	if (!fu_quirks_load(priv->quirks, flags, &error_local))
		g_warning("Failed to load quirks: %s", error_local->message);

	/* always */
	return TRUE;
}

/**
 * fu_context_get_power_state:
 * @self: a #FuContext
 *
 * Gets if the system is on battery power, e.g. UPS or laptop battery.
 *
 * Returns: a power state, e.g. %FU_POWER_STATE_BATTERY_DISCHARGING
 *
 * Since: 1.8.11
 **/
FuPowerState
fu_context_get_power_state(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	return priv->power_state;
}

/**
 * fu_context_set_power_state:
 * @self: a #FuContext
 * @power_state: a power state, e.g. %FU_POWER_STATE_BATTERY_DISCHARGING
 *
 * Sets if the system is on battery power, e.g. UPS or laptop battery.
 *
 * Since: 1.8.11
 **/
void
fu_context_set_power_state(FuContext *self, FuPowerState power_state)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	if (priv->power_state == power_state)
		return;
	priv->power_state = power_state;
	g_info("power state now %s", fu_power_state_to_string(power_state));
	g_object_notify(G_OBJECT(self), "power-state");
}

/**
 * fu_context_get_lid_state:
 * @self: a #FuContext
 *
 * Gets the laptop lid state, if applicable.
 *
 * Returns: a lid state, e.g. %FU_LID_STATE_CLOSED
 *
 * Since: 1.7.4
 **/
FuLidState
fu_context_get_lid_state(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	return priv->lid_state;
}

/**
 * fu_context_set_lid_state:
 * @self: a #FuContext
 * @lid_state: a lid state, e.g. %FU_LID_STATE_CLOSED
 *
 * Sets the laptop lid state, if applicable.
 *
 * Since: 1.7.4
 **/
void
fu_context_set_lid_state(FuContext *self, FuLidState lid_state)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	if (priv->lid_state == lid_state)
		return;
	priv->lid_state = lid_state;
	g_info("lid state now %s", fu_lid_state_to_string(lid_state));
	g_object_notify(G_OBJECT(self), "lid-state");
}

/**
 * fu_context_get_display_state:
 * @self: a #FuContext
 *
 * Gets the display state, if applicable.
 *
 * Returns: a display state, e.g. %FU_DISPLAY_STATE_CONNECTED
 *
 * Since: 1.9.6
 **/
FuDisplayState
fu_context_get_display_state(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	return priv->display_state;
}

/**
 * fu_context_set_display_state:
 * @self: a #FuContext
 * @display_state: a display state, e.g. %FU_DISPLAY_STATE_CONNECTED
 *
 * Sets the display state, if applicable.
 *
 * Since: 1.9.6
 **/
void
fu_context_set_display_state(FuContext *self, FuDisplayState display_state)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	if (priv->display_state == display_state)
		return;
	priv->display_state = display_state;
	g_info("display-state now %s", fu_display_state_to_string(display_state));
	g_object_notify(G_OBJECT(self), "display-state");
}

/**
 * fu_context_get_battery_level:
 * @self: a #FuContext
 *
 * Gets the system battery level in percent.
 *
 * Returns: percentage value, or %FWUPD_BATTERY_LEVEL_INVALID for unknown
 *
 * Since: 1.6.0
 **/
guint
fu_context_get_battery_level(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), G_MAXUINT);
	return priv->battery_level;
}

/**
 * fu_context_set_battery_level:
 * @self: a #FuContext
 * @battery_level: value
 *
 * Sets the system battery level in percent.
 *
 * Since: 1.6.0
 **/
void
fu_context_set_battery_level(FuContext *self, guint battery_level)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(battery_level <= FWUPD_BATTERY_LEVEL_INVALID);
	if (priv->battery_level == battery_level)
		return;
	priv->battery_level = battery_level;
	g_info("battery level now %u", battery_level);
	g_object_notify(G_OBJECT(self), "battery-level");
}

/**
 * fu_context_get_battery_threshold:
 * @self: a #FuContext
 *
 * Gets the system battery threshold in percent.
 *
 * Returns: percentage value, or %FWUPD_BATTERY_LEVEL_INVALID for unknown
 *
 * Since: 1.6.0
 **/
guint
fu_context_get_battery_threshold(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), G_MAXUINT);
	return priv->battery_threshold;
}

/**
 * fu_context_set_battery_threshold:
 * @self: a #FuContext
 * @battery_threshold: value
 *
 * Sets the system battery threshold in percent.
 *
 * Since: 1.6.0
 **/
void
fu_context_set_battery_threshold(FuContext *self, guint battery_threshold)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(battery_threshold <= FWUPD_BATTERY_LEVEL_INVALID);
	if (priv->battery_threshold == battery_threshold)
		return;
	priv->battery_threshold = battery_threshold;
	g_info("battery threshold now %u", battery_threshold);
	g_object_notify(G_OBJECT(self), "battery-threshold");
}

/**
 * fu_context_add_flag:
 * @context: a #FuContext
 * @flag: the context flag
 *
 * Adds a specific flag to the context.
 *
 * Since: 1.8.5
 **/
void
fu_context_add_flag(FuContext *context, FuContextFlags flag)
{
	FuContextPrivate *priv = GET_PRIVATE(context);
	g_return_if_fail(FU_IS_CONTEXT(context));
	if (priv->flags & flag)
		return;
	priv->flags |= flag;
	g_object_notify(G_OBJECT(context), "flags");
}

/**
 * fu_context_remove_flag:
 * @context: a #FuContext
 * @flag: the context flag
 *
 * Removes a specific flag from the context.
 *
 * Since: 1.8.10
 **/
void
fu_context_remove_flag(FuContext *context, FuContextFlags flag)
{
	FuContextPrivate *priv = GET_PRIVATE(context);
	g_return_if_fail(FU_IS_CONTEXT(context));
	if ((priv->flags & flag) == 0)
		return;
	priv->flags &= ~flag;
	g_object_notify(G_OBJECT(context), "flags");
}

/**
 * fu_context_has_flag:
 * @context: a #FuContext
 * @flag: the context flag
 *
 * Finds if the context has a specific flag.
 *
 * Returns: %TRUE if the flag is set
 *
 * Since: 1.8.5
 **/
gboolean
fu_context_has_flag(FuContext *context, FuContextFlags flag)
{
	FuContextPrivate *priv = GET_PRIVATE(context);
	g_return_val_if_fail(FU_IS_CONTEXT(context), FALSE);
	return (priv->flags & flag) > 0;
}

/**
 * fu_context_add_esp_volume:
 * @self: a #FuContext
 * @volume: a #FuVolume
 *
 * Adds an ESP volume location.
 *
 * Since: 1.8.5
 **/
void
fu_context_add_esp_volume(FuContext *self, FuVolume *volume)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(FU_IS_VOLUME(volume));

	/* check for dupes */
	for (guint i = 0; i < priv->esp_volumes->len; i++) {
		FuVolume *volume_tmp = g_ptr_array_index(priv->esp_volumes, i);
		if (g_strcmp0(fu_volume_get_id(volume_tmp), fu_volume_get_id(volume)) == 0) {
			g_debug("not adding duplicate volume %s", fu_volume_get_id(volume));
			return;
		}
	}

	/* add */
	g_ptr_array_add(priv->esp_volumes, g_object_ref(volume));
}

/**
 * fu_context_set_esp_location:
 * @self: A #FuContext object.
 * @location: The path to the preferred ESP.
 *
 * Sets the user's desired ESP (EFI System Partition) location for the given #FuContext.
 */
void
fu_context_set_esp_location(FuContext *self, const gchar *location)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(location != NULL);
	g_free(priv->esp_location);
	priv->esp_location = g_strdup(location);
}

/**
 * fu_context_get_esp_location:
 * @self: The FuContext object.
 *
 * Retrieves the user's desired ESP (EFI System Partition) location for the given #FuContext
 *
 * Return: The preferred ESP location as a string.
 */
const gchar *
fu_context_get_esp_location(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->esp_location;
}

/**
 * fu_context_get_esp_volumes:
 * @self: a #FuContext
 * @error: (nullable): optional return location for an error
 *
 * Finds all volumes that could be an ESP.
 *
 * The volumes are cached and so subsequent calls to this function will be much faster.
 *
 * Returns: (transfer container) (element-type FuVolume): a #GPtrArray, or %NULL if no ESP was found
 *
 * Since: 1.8.5
 **/
GPtrArray *
fu_context_get_esp_volumes(FuContext *self, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	const gchar *path_tmp;
	g_autoptr(GError) error_bdp = NULL;
	g_autoptr(GError) error_esp = NULL;
	g_autoptr(GPtrArray) volumes_bdp = NULL;
	g_autoptr(GPtrArray) volumes_esp = NULL;

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* cached result */
	if (priv->esp_volumes->len > 0)
		return g_ptr_array_ref(priv->esp_volumes);

	/* for the test suite use local directory for ESP */
	path_tmp = g_getenv("FWUPD_UEFI_ESP_PATH");
	if (path_tmp != NULL) {
		g_autoptr(FuVolume) vol = fu_volume_new_from_mount_path(path_tmp);
		fu_volume_set_partition_kind(vol, FU_VOLUME_KIND_ESP);
		fu_volume_set_partition_uuid(vol, "00000000-0000-0000-0000-000000000000");
		fu_context_add_esp_volume(self, vol);
		return g_ptr_array_ref(priv->esp_volumes);
	}

	/* ESP */
	volumes_esp = fu_volume_new_by_kind(FU_VOLUME_KIND_ESP, &error_esp);
	if (volumes_esp == NULL) {
		g_debug("%s", error_esp->message);
	} else {
		for (guint i = 0; i < volumes_esp->len; i++) {
			FuVolume *vol = g_ptr_array_index(volumes_esp, i);
			g_autofree gchar *type = fu_volume_get_id_type(vol);
			if (g_strcmp0(type, "vfat") != 0)
				continue;
			fu_context_add_esp_volume(self, vol);
		}
	}

	/* BDP */
	volumes_bdp = fu_volume_new_by_kind(FU_VOLUME_KIND_BDP, &error_bdp);
	if (volumes_bdp == NULL) {
		g_debug("%s", error_bdp->message);
	} else {
		for (guint i = 0; i < volumes_bdp->len; i++) {
			FuVolume *vol = g_ptr_array_index(volumes_bdp, i);
			g_autofree gchar *type = fu_volume_get_id_type(vol);
			if (g_strcmp0(type, "vfat") != 0)
				continue;
			if (!fu_volume_is_internal(vol))
				continue;
			fu_context_add_esp_volume(self, vol);
		}
	}

	/* nothing found */
	if (priv->esp_volumes->len == 0) {
		g_autoptr(GPtrArray) devices = NULL;

		/* check if udisks2 is working */
		devices = fu_common_get_block_devices(error);
		if (devices == NULL)
			return NULL;
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "No ESP or BDP found");
		return NULL;
	}

	/* success */
	return g_ptr_array_ref(priv->esp_volumes);
}

static gboolean
fu_context_is_esp(FuVolume *esp)
{
	g_autofree gchar *mount_point = fu_volume_get_mount_point(esp);
	g_autofree gchar *fn = NULL;
	g_autofree gchar *fn2 = NULL;

	if (mount_point == NULL)
		return FALSE;

	fn = g_build_filename(mount_point, "EFI", NULL);
	fn2 = g_build_filename(mount_point, "efi", NULL);

	return g_file_test(fn, G_FILE_TEST_IS_DIR) || g_file_test(fn2, G_FILE_TEST_IS_DIR);
}

static gboolean
fu_context_is_esp_linux(FuVolume *esp, GError **error)
{
	const gchar *prefixes[] = {"grub", "shim", "systemd-boot", "zfsbootmenu", NULL};
	g_autofree gchar *prefixes_str = NULL;
	g_autofree gchar *mount_point = fu_volume_get_mount_point(esp);
	g_autoptr(GPtrArray) files = NULL;

	/* look for any likely basenames */
	if (mount_point == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no mountpoint for ESP");
		return FALSE;
	}
	files = fu_path_get_files(mount_point, error);
	if (files == NULL)
		return FALSE;
	for (guint i = 0; i < files->len; i++) {
		const gchar *fn = g_ptr_array_index(files, i);
		g_autofree gchar *basename = g_path_get_basename(fn);
		g_autofree gchar *basename_lower = g_utf8_strdown(basename, -1);

		for (guint j = 0; prefixes[j] != NULL; j++) {
			if (!g_str_has_prefix(basename_lower, prefixes[j]))
				continue;
			if (!g_str_has_suffix(basename_lower, ".efi"))
				continue;
			g_info("found %s which indicates a Linux ESP, using %s", fn, mount_point);
			return TRUE;
		}
	}

	/* failed */
	prefixes_str = g_strjoinv("|", (gchar **)prefixes);
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_FOUND,
		    "did not any files with prefix %s in %s",
		    prefixes_str,
		    mount_point);
	return FALSE;
}

static gint
fu_context_sort_esp_score_cb(gconstpointer a, gconstpointer b, gpointer user_data)
{
	GHashTable *esp_scores = (GHashTable *)user_data;
	guint esp1_score = GPOINTER_TO_UINT(g_hash_table_lookup(esp_scores, *((FuVolume **)a)));
	guint esp2_score = GPOINTER_TO_UINT(g_hash_table_lookup(esp_scores, *((FuVolume **)b)));
	if (esp1_score < esp2_score)
		return 1;
	if (esp1_score > esp2_score)
		return -1;
	return 0;
}

/**
 * fu_context_get_default_esp:
 * @self: a #FuContext
 * @error: (nullable): optional return location for an error
 *
 * Finds the volume that represents the ESP that plugins should nominally
 * use for accessing storing data.
 *
 * Returns: (transfer full): a volume, or %NULL if no ESP was found
 *
 * Since: 2.0.0
 **/
FuVolume *
fu_context_get_default_esp(FuContext *self, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_autoptr(GPtrArray) esp_volumes = NULL;
	const gchar *user_esp_location = fu_context_get_esp_location(self);

	/* show which volumes we're choosing from */
	esp_volumes = fu_context_get_esp_volumes(self, error);
	if (esp_volumes == NULL)
		return NULL;

	/* no mounting */
	if (priv->flags & FU_CONTEXT_FLAG_INHIBIT_VOLUME_MOUNT) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "cannot mount volume by policy");
		return NULL;
	}

	/* we found more than one: lets look for the best one */
	if (esp_volumes->len > 1) {
		g_autoptr(GString) str = g_string_new("more than one ESP possible:");
		g_autoptr(GHashTable) esp_scores = g_hash_table_new(g_direct_hash, g_direct_equal);
		for (guint i = 0; i < esp_volumes->len; i++) {
			FuVolume *esp = g_ptr_array_index(esp_volumes, i);
			guint score = 0;
			g_autofree gchar *kind = NULL;
			g_autoptr(FuDeviceLocker) locker = NULL;
			g_autoptr(GError) error_local = NULL;

			/* ignore the volume completely if we cannot mount it */
			locker = fu_volume_locker(esp, &error_local);
			if (locker == NULL) {
				g_warning("failed to mount ESP: %s", error_local->message);
				continue;
			}

			/* if user specified, make sure that it matches */
			if (user_esp_location != NULL) {
				g_autofree gchar *mount = fu_volume_get_mount_point(esp);
				if (g_strcmp0(mount, user_esp_location) != 0) {
					g_debug("skipping %s as it's not the user "
						"specified ESP",
						mount);
					continue;
				}
			}

			if (!fu_context_is_esp(esp)) {
				g_debug("not an ESP: %s", fu_volume_get_id(esp));
				continue;
			}

			/* big partitions are better than small partitions */
			score += fu_volume_get_size(esp) / (1024 * 1024);

			/* prefer partitions with the ESP flag set over msftdata */
			kind = fu_volume_get_partition_kind(esp);
			if (g_strcmp0(kind, FU_VOLUME_KIND_ESP) == 0)
				score += 0x20000;

			/* prefer linux ESP */
			if (!fu_context_is_esp_linux(esp, &error_local)) {
				g_debug("not a Linux ESP: %s", error_local->message);
			} else {
				score += 0x10000;
			}
			g_hash_table_insert(esp_scores, (gpointer)esp, GUINT_TO_POINTER(score));
		}

		if (g_hash_table_size(esp_scores) == 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no EFI system partition found");
			return NULL;
		}

		g_ptr_array_sort_with_data(esp_volumes, fu_context_sort_esp_score_cb, esp_scores);
		for (guint i = 0; i < esp_volumes->len; i++) {
			FuVolume *esp = g_ptr_array_index(esp_volumes, i);
			guint score = GPOINTER_TO_UINT(g_hash_table_lookup(esp_scores, esp));
			g_string_append_printf(str, "\n - 0x%x:\t%s", score, fu_volume_get_id(esp));
		}
		g_debug("%s", str->str);
	}

	if (esp_volumes->len == 1) {
		FuVolume *esp = g_ptr_array_index(esp_volumes, 0);
		g_autoptr(FuDeviceLocker) locker = NULL;

		/* ensure it can be mounted */
		locker = fu_volume_locker(esp, error);
		if (locker == NULL)
			return NULL;

		/* if user specified, does it match mountpoints ? */
		if (user_esp_location != NULL) {
			g_autofree gchar *mount = fu_volume_get_mount_point(esp);

			if (g_strcmp0(mount, user_esp_location) != 0) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "user specified ESP %s not found",
					    user_esp_location);
				return NULL;
			}
		}
	}

	/* "success" */
	return g_object_ref(g_ptr_array_index(esp_volumes, 0));
}

/**
 * fu_context_get_esp_volume_by_hard_drive_device_path:
 * @self: a #FuContext
 * @dp: a #FuEfiHardDriveDevicePath
 * @error: (nullable): optional return location for an error
 *
 * Gets a volume that matches the EFI device path
 *
 * Returns: (transfer full): a volume, or %NULL if it was not found
 *
 * Since: 2.0.0
 **/
FuVolume *
fu_context_get_esp_volume_by_hard_drive_device_path(FuContext *self,
						    FuEfiHardDriveDevicePath *dp,
						    GError **error)
{
	g_autoptr(GPtrArray) volumes = NULL;

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(FU_IS_EFI_HARD_DRIVE_DEVICE_PATH(dp), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	volumes = fu_context_get_esp_volumes(self, error);
	if (volumes == NULL)
		return NULL;
	for (guint i = 0; i < volumes->len; i++) {
		FuVolume *volume = g_ptr_array_index(volumes, i);
		g_autoptr(GError) error_local = NULL;
		g_autoptr(FuEfiHardDriveDevicePath) dp_tmp = NULL;

		dp_tmp = fu_efi_hard_drive_device_path_new_from_volume(volume, &error_local);
		if (dp_tmp == NULL) {
			g_debug("%s", error_local->message);
			continue;
		}
		if (!fu_efi_hard_drive_device_path_compare(dp, dp_tmp))
			continue;
		return g_object_ref(volume);
	}

	/* failed */
	g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "could not find EFI DP");
	return NULL;
}

static FuFirmware *
fu_context_esp_load_pe_file(const gchar *filename, GError **error)
{
	g_autoptr(FuFirmware) firmware = fu_pefile_firmware_new();
	g_autoptr(GFile) file = g_file_new_for_path(filename);
	fu_firmware_set_filename(firmware, filename);
	if (!fu_firmware_parse_file(firmware, file, FU_FIRMWARE_PARSE_FLAG_NONE, error)) {
		g_prefix_error(error, "failed to load %s: ", filename);
		return NULL;
	}
	return g_steal_pointer(&firmware);
}

static gchar *
fu_context_build_uefi_basename_for_arch(const gchar *app_name)
{
#if defined(__x86_64__)
	return g_strdup_printf("%sx64.efi", app_name);
#endif
#if defined(__aarch64__)
	return g_strdup_printf("%saa64.efi", app_name);
#endif
#if defined(__loongarch_lp64)
	return g_strdup_printf("%sloongarch64.efi", app_name);
#endif
#if (defined(__riscv) && __riscv_xlen == 64)
	return g_strdup_printf("%sriscv64.efi", app_name);
#endif
#if defined(__i386__) || defined(__i686__)
	return g_strdup_printf("%sia32.efi", app_name);
#endif
#if defined(__arm__)
	return g_strdup_printf("%sarm.efi", app_name);
#endif
	return NULL;
}

static gboolean
fu_context_get_esp_files_for_entry(FuContext *self,
				   FuEfiLoadOption *entry,
				   GPtrArray *files,
				   FuContextEspFileFlags flags,
				   GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_autofree gchar *dp_filename = NULL;
	g_autofree gchar *filename = NULL;
	g_autofree gchar *mount_point = NULL;
	g_autofree gchar *shim_name = fu_context_build_uefi_basename_for_arch("shim");
	g_autoptr(FuDeviceLocker) volume_locker = NULL;
	g_autoptr(FuEfiFilePathDevicePath) dp_path = NULL;
	g_autoptr(FuEfiHardDriveDevicePath) dp_hdd = NULL;
	g_autoptr(FuFirmware) dp_list = NULL;
	g_autoptr(FuVolume) volume = NULL;

	/* all entries should have a list */
	dp_list =
	    fu_firmware_get_image_by_gtype(FU_FIRMWARE(entry), FU_TYPE_EFI_DEVICE_PATH_LIST, NULL);
	if (dp_list == NULL)
		return TRUE;

	/* HDD */
	dp_hdd = FU_EFI_HARD_DRIVE_DEVICE_PATH(
	    fu_firmware_get_image_by_gtype(FU_FIRMWARE(dp_list),
					   FU_TYPE_EFI_HARD_DRIVE_DEVICE_PATH,
					   NULL));
	if (dp_hdd == NULL)
		return TRUE;

	/* FILE */
	dp_path = FU_EFI_FILE_PATH_DEVICE_PATH(
	    fu_firmware_get_image_by_gtype(FU_FIRMWARE(dp_list),
					   FU_TYPE_EFI_FILE_PATH_DEVICE_PATH,
					   NULL));
	if (dp_path == NULL)
		return TRUE;

	/* can we match the volume? */
	volume = fu_context_get_esp_volume_by_hard_drive_device_path(self, dp_hdd, error);
	if (volume == NULL)
		return FALSE;
	if (priv->flags & FU_CONTEXT_FLAG_INHIBIT_VOLUME_MOUNT) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "cannot mount volume by policy");
		return FALSE;
	}
	volume_locker = fu_volume_locker(volume, error);
	if (volume_locker == NULL)
		return FALSE;
	dp_filename = fu_efi_file_path_device_path_get_name(dp_path, error);
	if (dp_filename == NULL)
		return FALSE;

	/* the file itself */
	mount_point = fu_volume_get_mount_point(volume);
	filename = g_build_filename(mount_point, dp_filename, NULL);
	g_debug("check for 1st stage bootloader: %s", filename);
	if (flags & FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_FIRST_STAGE) {
		g_autoptr(FuFirmware) firmware = NULL;
		g_autoptr(GError) error_local = NULL;

		/* ignore if the file cannot be loaded as a PE file */
		firmware = fu_context_esp_load_pe_file(filename, &error_local);
		if (firmware == NULL) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED) ||
			    g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE)) {
				g_debug("ignoring: %s", error_local->message);
			} else {
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
		} else {
			fu_firmware_set_idx(firmware, fu_firmware_get_idx(FU_FIRMWARE(entry)));
			g_ptr_array_add(files, g_steal_pointer(&firmware));
		}
	}

	/* the 2nd stage bootloader, typically grub */
	if (flags & FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_SECOND_STAGE &&
	    g_str_has_suffix(filename, shim_name)) {
		g_autoptr(FuFirmware) firmware = NULL;
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GString) filename2 = g_string_new(filename);
		const gchar *path;

		path =
		    fu_efi_load_option_get_metadata(entry, FU_EFI_LOAD_OPTION_METADATA_PATH, NULL);
		if (path != NULL) {
			g_string_replace(filename2, shim_name, path, 1);
		} else {
			g_autofree gchar *grub_name =
			    fu_context_build_uefi_basename_for_arch("grub");
			g_string_replace(filename2, shim_name, grub_name, 1);
		}
		g_debug("check for 2nd stage bootloader: %s", filename2->str);

		/* ignore if the file cannot be loaded as a PE file */
		firmware = fu_context_esp_load_pe_file(filename2->str, &error_local);
		if (firmware == NULL) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED) ||
			    g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE)) {
				g_debug("ignoring: %s", error_local->message);
			} else {
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
		} else {
			fu_firmware_set_idx(firmware, fu_firmware_get_idx(FU_FIRMWARE(entry)));
			g_ptr_array_add(files, g_steal_pointer(&firmware));
		}
	}

	/* revocations, typically for SBAT */
	if (flags & FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_REVOCATIONS &&
	    g_str_has_suffix(filename, shim_name)) {
		g_autoptr(GString) filename2 = g_string_new(filename);
		g_autoptr(FuFirmware) firmware = NULL;
		g_autoptr(GError) error_local = NULL;

		g_string_replace(filename2, shim_name, "revocations.efi", 1);
		g_debug("check for revocation: %s", filename2->str);

		/* ignore if the file cannot be loaded as a PE file */
		firmware = fu_context_esp_load_pe_file(filename2->str, &error_local);
		if (firmware == NULL) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED) ||
			    g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE)) {
				g_debug("ignoring: %s", error_local->message);
			} else {
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
		} else {
			fu_firmware_set_idx(firmware, fu_firmware_get_idx(FU_FIRMWARE(entry)));
			g_ptr_array_add(files, g_steal_pointer(&firmware));
		}
	}

	/* success */
	return TRUE;
}

/**
 * fu_context_get_esp_files:
 * @self: a #FuContext
 * @flags: some #FuContextEspFileFlags, e.g. #FU_CONTEXT_ESP_FILE_FLAG_INCLUDE_FIRST_STAGE
 * @error: #GError
 *
 * Gets the PE files for all the entries listed in `BootOrder`.
 *
 * Returns: (transfer full) (element-type FuPefileFirmware): PE firmware data
 *
 * Since: 2.0.0
 **/
GPtrArray *
fu_context_get_esp_files(FuContext *self, FuContextEspFileFlags flags, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	entries = fu_efivars_get_boot_entries(priv->efivars, error);
	if (entries == NULL)
		return NULL;
	for (guint i = 0; i < entries->len; i++) {
		FuEfiLoadOption *entry = g_ptr_array_index(entries, i);
		g_autoptr(GError) error_local = NULL;
		if (!fu_context_get_esp_files_for_entry(self, entry, files, flags, &error_local)) {
			if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND) ||
			    g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE)) {
				g_debug("ignoring %s: %s",
					fu_firmware_get_id(FU_FIRMWARE(entry)),
					error_local->message);
				continue;
			}
			g_propagate_error(error, g_steal_pointer(&error_local));
			return NULL;
		}
	}

	/* success */
	return g_steal_pointer(&files);
}

/**
 * fu_context_get_backends:
 * @self: a #FuContext
 *
 * Gets all the possible backends used by all plugins.
 *
 * Returns: (transfer none) (element-type FuBackend): List of backends
 *
 * Since: 2.0.0
 **/
GPtrArray *
fu_context_get_backends(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	return priv->backends;
}

/**
 * fu_context_add_backend:
 * @self: a #FuContext
 * @backend: a #FuBackend
 *
 * Adds a backend to the context.
 *
 * Returns: (transfer full): a #FuBackend, or %NULL on error
 *
 * Since: 2.0.0
 **/
void
fu_context_add_backend(FuContext *self, FuBackend *backend)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(FU_IS_BACKEND(backend));
	g_ptr_array_add(priv->backends, g_object_ref(backend));
}

/**
 * fu_context_get_backend_by_name:
 * @self: a #FuContext
 * @name: backend name, e.g. `udev` or `bluez`
 * @error: (nullable): optional return location for an error
 *
 * Gets a specific backend added to the context.
 *
 * Returns: (transfer full): a #FuBackend, or %NULL on error
 *
 * Since: 2.0.0
 **/
FuBackend *
fu_context_get_backend_by_name(FuContext *self, const gchar *name, GError **error)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	for (guint i = 0; i < priv->backends->len; i++) {
		FuBackend *backend = g_ptr_array_index(priv->backends, i);
		if (g_strcmp0(fu_backend_get_name(backend), name) == 0)
			return g_object_ref(backend);
	}
	g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no backend with name %s", name);
	return NULL;
}

/* private */
gboolean
fu_context_has_backend(FuContext *self, const gchar *name)
{
	FuContextPrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	for (guint i = 0; i < priv->backends->len; i++) {
		FuBackend *backend = g_ptr_array_index(priv->backends, i);
		if (g_strcmp0(fu_backend_get_name(backend), name) == 0)
			return TRUE;
	}
	return FALSE;
}

/* private */
gpointer
fu_context_get_data(FuContext *self, const gchar *key)
{
	g_return_val_if_fail(FU_IS_CONTEXT(self), NULL);
	g_return_val_if_fail(key != NULL, NULL);
	return g_object_get_data(G_OBJECT(self), key);
}

/* private */
void
fu_context_set_data(FuContext *self, const gchar *key, gpointer data)
{
	g_return_if_fail(FU_IS_CONTEXT(self));
	g_return_if_fail(key != NULL);
	g_object_set_data(G_OBJECT(self), key, data);
}

static void
fu_context_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	FuContext *self = FU_CONTEXT(object);
	FuContextPrivate *priv = GET_PRIVATE(self);
	switch (prop_id) {
	case PROP_POWER_STATE:
		g_value_set_uint(value, priv->power_state);
		break;
	case PROP_LID_STATE:
		g_value_set_uint(value, priv->lid_state);
		break;
	case PROP_DISPLAY_STATE:
		g_value_set_uint(value, priv->display_state);
		break;
	case PROP_BATTERY_LEVEL:
		g_value_set_uint(value, priv->battery_level);
		break;
	case PROP_BATTERY_THRESHOLD:
		g_value_set_uint(value, priv->battery_threshold);
		break;
	case PROP_FLAGS:
		g_value_set_uint64(value, priv->flags);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_context_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	FuContext *self = FU_CONTEXT(object);
	FuContextPrivate *priv = GET_PRIVATE(self);
	switch (prop_id) {
	case PROP_POWER_STATE:
		fu_context_set_power_state(self, g_value_get_uint(value));
		break;
	case PROP_LID_STATE:
		fu_context_set_lid_state(self, g_value_get_uint(value));
		break;
	case PROP_DISPLAY_STATE:
		fu_context_set_display_state(self, g_value_get_uint(value));
		break;
	case PROP_BATTERY_LEVEL:
		fu_context_set_battery_level(self, g_value_get_uint(value));
		break;
	case PROP_BATTERY_THRESHOLD:
		fu_context_set_battery_threshold(self, g_value_get_uint(value));
		break;
	case PROP_FLAGS:
		priv->flags = g_value_get_uint64(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_context_dispose(GObject *object)
{
	FuContext *self = FU_CONTEXT(object);
	FuContextPrivate *priv = GET_PRIVATE(self);
	g_ptr_array_set_size(priv->backends, 0);
	G_OBJECT_CLASS(fu_context_parent_class)->dispose(object);
}

static void
fu_context_finalize(GObject *object)
{
	FuContext *self = FU_CONTEXT(object);
	FuContextPrivate *priv = GET_PRIVATE(self);

	if (priv->fdt != NULL)
		g_object_unref(priv->fdt);
	if (priv->efivars != NULL)
		g_object_unref(priv->efivars);
	g_free(priv->esp_location);
	g_hash_table_unref(priv->runtime_versions);
	g_hash_table_unref(priv->compile_versions);
	g_object_unref(priv->hwids);
	g_object_unref(priv->config);
	g_hash_table_unref(priv->hwid_flags);
	g_object_unref(priv->quirks);
	g_object_unref(priv->smbios);
	g_object_unref(priv->host_bios_settings);
	g_hash_table_unref(priv->firmware_gtypes);
	g_hash_table_unref(priv->udev_subsystems);
	g_ptr_array_unref(priv->esp_volumes);
	g_ptr_array_unref(priv->backends);

	G_OBJECT_CLASS(fu_context_parent_class)->finalize(object);
}

static void
fu_context_class_init(FuContextClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec;

	object_class->dispose = fu_context_dispose;
	object_class->get_property = fu_context_get_property;
	object_class->set_property = fu_context_set_property;

	/**
	 * FuContext:power-state:
	 *
	 * The system power state.
	 *
	 * Since: 1.8.11
	 */
	pspec = g_param_spec_uint("power-state",
				  NULL,
				  NULL,
				  FU_POWER_STATE_UNKNOWN,
				  FU_POWER_STATE_LAST,
				  FU_POWER_STATE_UNKNOWN,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_POWER_STATE, pspec);

	/**
	 * FuContext:lid-state:
	 *
	 * The system lid state.
	 *
	 * Since: 1.7.4
	 */
	pspec = g_param_spec_uint("lid-state",
				  NULL,
				  NULL,
				  FU_LID_STATE_UNKNOWN,
				  FU_LID_STATE_LAST,
				  FU_LID_STATE_UNKNOWN,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_LID_STATE, pspec);

	/**
	 * FuContext:display-state:
	 *
	 * The display state.
	 *
	 * Since: 1.9.6
	 */
	pspec = g_param_spec_uint("display-state",
				  NULL,
				  NULL,
				  FU_DISPLAY_STATE_UNKNOWN,
				  FU_DISPLAY_STATE_LAST,
				  FU_DISPLAY_STATE_UNKNOWN,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_DISPLAY_STATE, pspec);

	/**
	 * FuContext:battery-level:
	 *
	 * The system battery level in percent.
	 *
	 * Since: 1.6.0
	 */
	pspec = g_param_spec_uint("battery-level",
				  NULL,
				  NULL,
				  0,
				  FWUPD_BATTERY_LEVEL_INVALID,
				  FWUPD_BATTERY_LEVEL_INVALID,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_BATTERY_LEVEL, pspec);

	/**
	 * FuContext:battery-threshold:
	 *
	 * The system battery threshold in percent.
	 *
	 * Since: 1.6.0
	 */
	pspec = g_param_spec_uint("battery-threshold",
				  NULL,
				  NULL,
				  0,
				  FWUPD_BATTERY_LEVEL_INVALID,
				  FWUPD_BATTERY_LEVEL_INVALID,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_BATTERY_THRESHOLD, pspec);

	/**
	 * FuContext:flags:
	 *
	 * The context flags.
	 *
	 * Since: 1.8.10
	 */
	pspec = g_param_spec_uint64("flags",
				    NULL,
				    NULL,
				    FU_CONTEXT_FLAG_NONE,
				    G_MAXUINT64,
				    FU_CONTEXT_FLAG_NONE,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_FLAGS, pspec);

	/**
	 * FuContext::security-changed:
	 * @self: the #FuContext instance that emitted the signal
	 *
	 * The ::security-changed signal is emitted when some system state has changed that could
	 * have affected the security level.
	 *
	 * Since: 1.6.0
	 **/
	signals[SIGNAL_SECURITY_CHANGED] =
	    g_signal_new("security-changed",
			 G_TYPE_FROM_CLASS(object_class),
			 G_SIGNAL_RUN_LAST,
			 G_STRUCT_OFFSET(FuContextClass, security_changed),
			 NULL,
			 NULL,
			 g_cclosure_marshal_VOID__VOID,
			 G_TYPE_NONE,
			 0);
	/**
	 * FuContext::housekeeping:
	 * @self: the #FuContext instance that emitted the signal
	 *
	 * The ::housekeeping signal is emitted when helper objects should do house-keeping actions
	 * when the daemon is idle.
	 *
	 * Since: 2.0.0
	 **/
	signals[SIGNAL_HOUSEKEEPING] = g_signal_new("housekeeping",
						    G_TYPE_FROM_CLASS(object_class),
						    G_SIGNAL_RUN_LAST,
						    G_STRUCT_OFFSET(FuContextClass, housekeeping),
						    NULL,
						    NULL,
						    g_cclosure_marshal_VOID__VOID,
						    G_TYPE_NONE,
						    0);

	object_class->finalize = fu_context_finalize;
}

static void
fu_context_init(FuContext *self)
{
	FuContextPrivate *priv = GET_PRIVATE(self);
	priv->chassis_kind = FU_SMBIOS_CHASSIS_KIND_UNKNOWN;
	priv->battery_level = FWUPD_BATTERY_LEVEL_INVALID;
	priv->battery_threshold = FWUPD_BATTERY_LEVEL_INVALID;
	priv->smbios = fu_smbios_new();
	priv->hwids = fu_hwids_new();
	priv->config = fu_config_new();
	priv->efivars = g_strcmp0(g_getenv("FWUPD_EFIVARS"), "dummy") == 0 ? fu_dummy_efivars_new()
									   : fu_efivars_new();
	priv->hwid_flags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	priv->udev_subsystems = g_hash_table_new_full(g_str_hash,
						      g_str_equal,
						      g_free,
						      (GDestroyNotify)g_ptr_array_unref);
	priv->firmware_gtypes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	priv->quirks = fu_quirks_new(self);
	priv->host_bios_settings = fu_bios_settings_new();
	priv->esp_volumes = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	priv->runtime_versions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	priv->compile_versions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	priv->backends = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
}

/**
 * fu_context_new:
 *
 * Creates a new #FuContext
 *
 * Returns: (transfer full): the object
 *
 * Since: 1.6.0
 **/
FuContext *
fu_context_new(void)
{
	return FU_CONTEXT(g_object_new(FU_TYPE_CONTEXT, NULL));
}
