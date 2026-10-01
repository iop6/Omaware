// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshothistory.h"
#include <QSet>

namespace {
QVariantMap snapshot(const QVariantList &items, const QString &id) {
    for (const auto &v : items) if (v.toMap()["id"].toString() == id) return v.toMap();
    return {};
}
}

QVariantMap SnapshotHistory::view(const QVariantList &items, const QVariantMap &marker) {
    // Saved snapshots form the tree. The current VM is a separate, mutable
    // child of marker.id; choosing a node in the UI never changes this marker.
    const auto current = snapshot(items, marker["id"].toString());
    return {{"items", items}, {"currentId", current["id"]}};
}

QVariantMap SnapshotHistory::captured(QVariantMap marker, const QVariantMap &item) {
    // Retain older labels and unknown metadata without interpreting them. Named
    // branch registries no longer control snapshot ancestry.
    marker["id"] = item["id"]; marker["name"] = item["name"]; marker["action"] = "captured";
    return marker;
}

QVariantMap SnapshotHistory::restored(const QVariantList &items, QVariantMap marker, QString id, QString &error) {
    const auto target = snapshot(items, id);
    if (target.isEmpty()) { error = "The selected snapshot no longer exists."; return {}; }
    marker["id"] = id; marker["name"] = target["name"]; marker["action"] = "restored";
    return marker;
}

void SnapshotHistory::removed(QVariantMap &marker, const QVariantMap &item) {
    // Publish the deletion and current-parent change together. Immutable child
    // manifests resolve these redirects even after the deleted files are gone.
    auto deleted = marker["deletedSnapshots"].toMap();
    deleted[item["id"].toString()] = QVariantMap{{"parentId", item["parentId"]}, {"name", item["name"]}};
    marker["deletedSnapshots"] = deleted;
    if (marker["id"] == item["id"]) {
        marker["id"] = item["parentId"]; marker["name"] = item["parent"];
        marker["action"] = "removed";
    }
}

QString SnapshotHistory::survivingParent(const QVariantMap &marker, QString id) {
    const auto deleted = marker["deletedSnapshots"].toMap(); QSet<QString> seen;
    while (deleted.contains(id)) {
        if (seen.contains(id)) return {};
        seen.insert(id); id = deleted[id].toMap()["parentId"].toString();
    }
    return id;
}
