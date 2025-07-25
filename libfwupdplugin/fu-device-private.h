/*
 * Copyright 2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <xmlb.h>

#include "fu-backend.h"
#include "fu-device.h"

#define fu_device_set_plugin(d, v) fwupd_device_set_plugin(FWUPD_DEVICE(d), v)

gboolean
fu_device_has_private_flag_quark(FuDevice *self, GQuark flag_quark) G_GNUC_NON_NULL(1);
void
fu_device_remove_children(FuDevice *self) G_GNUC_NON_NULL(1);
GPtrArray *
fu_device_get_parent_guids(FuDevice *self) G_GNUC_NON_NULL(1);
gboolean
fu_device_has_parent_guid(FuDevice *self, const gchar *guid) G_GNUC_NON_NULL(1, 2);
GPtrArray *
fu_device_get_parent_physical_ids(FuDevice *self) G_GNUC_NON_NULL(1);
gboolean
fu_device_has_parent_physical_id(FuDevice *self, const gchar *physical_id) G_GNUC_NON_NULL(1, 2);
GPtrArray *
fu_device_get_parent_backend_ids(FuDevice *self) G_GNUC_NON_NULL(1);
gboolean
fu_device_has_parent_backend_id(FuDevice *self, const gchar *backend_id) G_GNUC_NON_NULL(1, 2);
void
fu_device_set_parent(FuDevice *self, FuDevice *parent) G_GNUC_NON_NULL(1);
gint
fu_device_get_order(FuDevice *self) G_GNUC_NON_NULL(1);
void
fu_device_set_order(FuDevice *self, gint order) G_GNUC_NON_NULL(1);
const gchar *
fu_device_get_update_request_id(FuDevice *self) G_GNUC_NON_NULL(1);
void
fu_device_set_update_request_id(FuDevice *self, const gchar *update_request_id) G_GNUC_NON_NULL(1);
const gchar *
fu_device_get_fwupd_version(FuDevice *self) G_GNUC_NON_NULL(1);
void
fu_device_set_fwupd_version(FuDevice *self, const gchar *fwupd_version) G_GNUC_NON_NULL(1, 2);
gboolean
fu_device_ensure_id(FuDevice *self, GError **error) G_GNUC_WARN_UNUSED_RESULT G_GNUC_NON_NULL(1);
void
fu_device_incorporate_from_component(FuDevice *self, XbNode *component) G_GNUC_NON_NULL(1, 2);
void
fu_device_replace(FuDevice *self, FuDevice *donor) G_GNUC_NON_NULL(1);
void
fu_device_ensure_from_component(FuDevice *self, XbNode *component) G_GNUC_NON_NULL(1, 2);
void
fu_device_ensure_from_release(FuDevice *self, XbNode *rel) G_GNUC_NON_NULL(1, 2);
void
fu_device_convert_instance_ids(FuDevice *self) G_GNUC_NON_NULL(1);
GPtrArray *
fu_device_get_possible_plugins(FuDevice *self) G_GNUC_NON_NULL(1);
guint
fu_device_get_request_cnt(FuDevice *self, FwupdRequestKind request_kind) G_GNUC_NON_NULL(1);
void
fu_device_set_progress(FuDevice *self, FuProgress *progress) G_GNUC_NON_NULL(1);
gboolean
fu_device_set_quirk_kv(FuDevice *self,
		       const gchar *key,
		       const gchar *value,
		       FuContextQuirkSource source,
		       GError **error) G_GNUC_NON_NULL(1, 2, 3);
void
fu_device_set_specialized_gtype(FuDevice *self, GType gtype) G_GNUC_NON_NULL(1);
void
fu_device_set_proxy_gtype(FuDevice *self, GType gtype) G_GNUC_NON_NULL(1);
GPtrArray *
fu_device_get_counterpart_guids(FuDevice *self) G_GNUC_NON_NULL(1);
gboolean
fu_device_is_updatable(FuDevice *self) G_GNUC_NON_NULL(1);
const gchar *
fu_device_get_custom_flags(FuDevice *self) G_GNUC_NON_NULL(1);
void
fu_device_set_custom_flags(FuDevice *self, const gchar *custom_flags) G_GNUC_NON_NULL(1);

void
fu_device_add_event(FuDevice *self, FuDeviceEvent *event);
void
fu_device_clear_events(FuDevice *self);
GPtrArray *
fu_device_get_events(FuDevice *self);
void
fu_device_set_target(FuDevice *self, FuDevice *target);

FuBackend *
fu_device_get_backend(FuDevice *self);
void
fu_device_set_backend(FuDevice *self, FuBackend *backend);

void
fu_device_add_json(FuDevice *self, JsonBuilder *builder, FwupdCodecFlags flags)
    G_GNUC_NON_NULL(1, 2);
gboolean
fu_device_from_json(FuDevice *self, JsonObject *json_object, GError **error) G_GNUC_NON_NULL(1, 2);
