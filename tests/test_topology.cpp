// SPDX-License-Identifier: GPL-3.0-or-later
// The actual network map QML with an inert backend and a fixed lab: no libvirt, no host changes.
#include <QtTest>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include "theme.h"
#include <QDomDocument>
#include <QJSValue>
#include <QJSValueIterator>
#include <QQmlExpression>

class MapBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList domains MEMBER domains CONSTANT)
    Q_PROPERTY(bool connected MEMBER connected CONSTANT)
    Q_PROPERTY(bool busy MEMBER busy CONSTANT)
public:
    QVariantList domains;
    bool connected = true, busy = false;
    QVariantList links; bool linksUp = false;
    Q_INVOKABLE void setLinks(const QVariantList &targets, bool up) { links = targets; linksUp = up; }
    Q_INVOKABLE bool configureNetwork(const QString &, const QString &, const QString &, const QString &, bool, bool, const QString &) { return true; }
signals:
    void linksSet(bool ok, QString message);
    void networkConfigured(QString uuid, bool ok, QString message);
};
class MapPreferences : public QObject {
    Q_OBJECT
public:
    QVariantMap values; QString copied;
    Q_INVOKABLE QVariant get(const QString &key, const QVariant &fallback = {}) { return values.value(key, fallback); }
    Q_INVOKABLE void set(const QString &key, const QVariant &value) { values[key] = value; }
    Q_INVOKABLE void copy(const QString &text) { copied = text; }
    Q_INVOKABLE QString localPath(const QString &url) const { return url.startsWith("file:") ? QUrl(url).toLocalFile() : url; }
    Q_INVOKABLE bool saveText(const QString &url, const QString &text) const { QFile f(localPath(url)); return f.open(QIODevice::WriteOnly) && f.write(text.toUtf8()) >= 0; }
    Q_INVOKABLE QString guide(const QString &name) const { QFile f(QString(OMAWARE_SOURCE_DIR) + "/docs/" + name); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }
};

class TopologyTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    MapBackend backend; MapPreferences preferences;
    std::unique_ptr<Theme> theme;
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QObject *map = nullptr, *fleet = nullptr;
    QQuickWindow *window = nullptr;

    static QVariantMap nic(const QString &mac, const QString &network, bool up, const QString &target = {}) {
        return {{"mac", mac}, {"networkId", network}, {"linkUp", up}, {"model", "virtio"}, {"target", target}};
    }
    // Internet + NAT network with a web server, an isolated analysis network with REMnux and FLARE
    // (FLARE also has a pulled cable to a private internet connection), and a stopped VM.
    QVariantMap catalog() {
        const QVariantList choices{
            QVariantMap{{"id", "bridge:omanat0001"}, {"kind", "bridge"}, {"source", "omanat0001"}, {"category", "NAT"}, {"displayName", "lab-nat"}, {"available", true}},
            QVariantMap{{"id", "bridge:omaiso0002"}, {"kind", "bridge"}, {"source", "omaiso0002"}, {"category", "Isolated"}, {"displayName", "analysis"}, {"available", true}, {"isolation", QVariantMap{{"isolated", true}}}}};
        const QVariantList items{
            QVariantMap{{"name", "omaware-lab-nat"}, {"title", "lab-nat"}, {"bridge", "omanat0001"}, {"active", true}, {"managed", true}, {"cidr", "10.20.0.1/24"}},
            QVariantMap{{"name", "omaware-analysis"}, {"title", "analysis"}, {"bridge", "omaiso0002"}, {"active", true}, {"managed", true}, {"isolation", QVariantMap{{"isolated", true}}}}};
        auto vm = [](const QString &uuid, const QString &name, bool active, const QVariantList &nics, const QVariantMap &addresses = {}) {
            return QVariantMap{{"uuid", uuid}, {"name", name}, {"active", active}, {"interfaces", nics}, {"liveInterfaces", active ? nics : QVariantList{}}, {"revision", "r"}, {"addresses", addresses}};
        };
        const QVariantList topology{
            vm("11111111-0000-4000-8000-000000000001", "omaware-web", true, {nic("52:54:00:00:00:01", "bridge:omanat0001", true, "vnet1")}, {{"52:54:00:00:00:01", QStringList{"10.20.0.10"}}}),
            vm("22222222-0000-4000-8000-000000000002", "omaware-remnux", true, {nic("52:54:00:00:00:02", "bridge:omaiso0002", true, "vnet2")}),
            vm("33333333-0000-4000-8000-000000000003", "omaware-flare", true, {nic("52:54:00:00:00:03", "bridge:omaiso0002", true, "vnet3"), nic("52:54:00:00:00:04", "user", false, "")}),
            vm("44444444-0000-4000-8000-000000000004", "omaware-old", false, {nic("52:54:00:00:00:05", "bridge:omanat0001", true)})};
        return {{"choices", choices}, {"items", items}, {"topology", topology}};
    }
    QVariantMap sample(double t, double scale) {
        auto row = [&](const QString &uuid, const QVariantList &nics) {
            double rx = 0, tx = 0; for (const auto &n : nics) { rx += n.toMap()["rxBytes"].toDouble(); tx += n.toMap()["txBytes"].toDouble(); }
            return QVariantMap{{"uuid", uuid}, {"cpuTime", t * 1e9}, {"vcpus", 2}, {"rxBytes", rx}, {"txBytes", tx}, {"nics", nics}};
        };
        auto n = [&](int index, const QString &name, double rx, double tx) {
            return QVariantMap{{"index", index}, {"name", name}, {"rxBytes", rx * scale}, {"txBytes", tx * scale}, {"rxPkts", rx * scale / 900}, {"txPkts", tx * scale / 900}, {"errors", 0}};
        };
        return {{"host", QVariantMap{{"sampledAt", t * 1000}, {"cpus", 8}}},
                {"vms", QVariantList{row("11111111-0000-4000-8000-000000000001", {n(0, "vnet1", 2e6, 4e5)}),
                                     row("22222222-0000-4000-8000-000000000002", {n(0, "vnet2", 3e3, 1e3)}),
                                     row("33333333-0000-4000-8000-000000000003", {n(0, "vnet3", 9e5, 6e5), n(1, "", 0, 0)})}}};
    }
    bool shot(const QString &name) {
        QTest::qWait(250);
        const auto out = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR");
        return out.isEmpty() || window->grabWindow().save(out + "/" + name + ".png");
    }
    static QVariant plain(QVariant v) { return v.canConvert<QJSValue>() && v.userType() == qMetaTypeId<QJSValue>() ? v.value<QJSValue>().toVariant() : v; }
    // Checks every cable and uplink: straight horizontal and vertical runs, never through a device other
    // than its two ends, and its own port where it plugs into a device. Returns how many it checked.
    int checkRoutes() {
        const auto graph = map->property("graph").toMap();
        QMap<QString, QSet<int>> taken; int checked = 0;
        QVariantList all;
        for (const auto &c : graph["cables"].toList()) all << QVariantMap{{"id", c.toMap()["id"]}, {"from", c.toMap()["vm"]}, {"to", c.toMap()["to"]}};
        for (const auto &u : graph["uplinks"].toList()) all << QVariantMap{{"id", u.toMap()["id"]}, {"from", u.toMap()["from"]}, {"to", u.toMap()["to"]}};
        for (const auto &entry : all) {
            const auto e = entry.toMap();
            QVariant r; if (!QMetaObject::invokeMethod(map, "ports", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, e["id"]))) return -1;
            const auto route = plain(r).toMap();
            const auto raw = route["raw"].toList();
            if (raw.size() < 2) { qWarning() << "no route" << e["id"]; return -1; }
            for (int i = 1; i < raw.size(); ++i) {
                const auto p = raw[i - 1].toMap(), q = raw[i].toMap();
                const bool loose = route["b"].isNull() || !route["b"].isValid();
                if (!loose && qAbs(p["x"].toDouble() - q["x"].toDouble()) > .01 && qAbs(p["y"].toDouble() - q["y"].toDouble()) > .01) { qWarning() << "diagonal run" << e["id"]; return -1; }
            }
            QVariantMap skip; skip[e["from"].toString()] = true; skip[e["to"].toString()] = true;
            QVariant n; QMetaObject::invokeMethod(map, "hits", Q_RETURN_ARG(QVariant, n), Q_ARG(QVariant, raw), Q_ARG(QVariant, skip));
            if (n.toInt() != 0) { qWarning() << "runs through a device" << e["id"]; return -1; }
            if (route["b"].isValid() && !route["b"].isNull()) {
                const auto key = e["to"].toString() + "|" + route["te"].toString();
                const auto b = route["b"].toMap(); const int at = qRound(b["x"].toDouble() * 7 + b["y"].toDouble());
                if (taken[key].contains(at)) { qWarning() << "shares a port" << e["id"]; return -1; }
                taken[key] << at;
            }
            ++checked;
        }
        return checked;
    }
    void showCatalog(const QVariantMap &data) {
        map->setProperty("catalog", data);
        QTest::qWait(200);
        QVERIFY(QMetaObject::invokeMethod(map, "arrange"));
        QTest::qWait(200);
    }
private slots:
    void initTestCase() {
        theme = std::make_unique<Theme>(dir.filePath("absent.toml"));
        backend.domains = {
            QVariantMap{{"uuid", "11111111-0000-4000-8000-000000000001"}, {"name", "omaware-web"}, {"stateCode", 1}, {"state", "Running"}, {"owned", true}},
            QVariantMap{{"uuid", "22222222-0000-4000-8000-000000000002"}, {"name", "omaware-remnux"}, {"stateCode", 1}, {"state", "Running"}, {"owned", true}},
            QVariantMap{{"uuid", "33333333-0000-4000-8000-000000000003"}, {"name", "omaware-flare"}, {"stateCode", 1}, {"state", "Running"}, {"owned", true}, {"contained", true}},
            QVariantMap{{"uuid", "44444444-0000-4000-8000-000000000004"}, {"name", "omaware-old"}, {"stateCode", 5}, {"state", "Shut off"}, {"owned", true}}};
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("preferences", &preferences);
        engine.rootContext()->setContextProperty("theme", theme.get());
        engine.rootContext()->setContextProperty("catalogData", catalog());
        QQmlComponent component(&engine);
        const QByteArray qml = "import QtQuick\nimport QtQuick.Controls\nimport \"" + QUrl::fromLocalFile(QString(OMAWARE_SOURCE_DIR) + "/qml").toString().toUtf8() + "\" as Ui\n"
            "ApplicationWindow { width: 1440; height: 900; visible: true; color: theme.colors.background\n"
            "  palette.window: theme.colors.background; palette.windowText: theme.colors.foreground; palette.text: theme.colors.foreground\n"
            "  palette.button: theme.colors.surface; palette.buttonText: theme.colors.foreground; palette.base: theme.colors.field\n"
            "  Ui.FleetStats { id: stats; objectName: \"fleet\" }\n"
            "  Ui.NetworkTopology { objectName: \"map\"; anchors.fill: parent; anchors.margins: 12; catalog: catalogData; fleet: stats } }";
        component.setData(qml, QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        root.reset(component.create()); QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        map = root->findChild<QObject *>("map"); fleet = root->findChild<QObject *>("fleet"); QVERIFY(map && fleet);
        QTRY_VERIFY(map->property("registry").toInt() >= 7);
        fleet->setProperty("sample", sample(100, 1)); fleet->setProperty("sample", sample(102, 2));
        QTest::qWait(300);
    }
    // Cables and uplinks run like circuit-board traces between ports of their own (a pulled cable is a
    // short loose end), and nothing runs through another device.
    void cleanRoutes() {
        QVERIFY(QMetaObject::invokeMethod(map, "arrange"));
        QTest::qWait(150);
        QCOMPARE(checkRoutes(), 7);
        // Internet sits right above This computer, so their link is one vertical line.
        QVariant r; QVERIFY(QMetaObject::invokeMethod(map, "ports", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, "host>internet")));
        const auto pts = plain(r).toMap()["raw"].toList();
        QCOMPARE(pts.size(), 2); QCOMPARE(pts[0].toMap()["x"].toDouble(), pts[1].toMap()["x"].toDouble());
        QVERIFY(shot("map-overview"));
    }
    // Clicking or dragging a box moves only that box: the view stays put, during the drag and after
    // the next refresh of the map's data.
    void dragMovesOnlyTheBox() {
        QVERIFY(QMetaObject::invokeMethod(map, "refit"));
        QTest::qWait(150);
        const auto items = map->property("items").value<QJSValue>();
        auto box = qobject_cast<QQuickItem *>(items.property("vm:22222222-0000-4000-8000-000000000002").toQObject()); QVERIFY(box);
        const double panX = map->property("panX").toDouble(), panY = map->property("panY").toDouble(), zoom = map->property("zoom").toDouble();
        const QPointF before(box->x(), box->y());
        auto view = [&]() { return QString("%1,%2,%3").arg(map->property("panX").toDouble()).arg(map->property("panY").toDouble()).arg(map->property("zoom").toDouble()); };
        const auto still = QString("%1,%2,%3").arg(panX).arg(panY).arg(zoom);
        // A click, with the small wobble a real click has.
        const QPoint at = box->mapToScene({60, 50}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(window, at + QPoint(3, 2)); QTest::qWait(16);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at + QPoint(3, 2));
        QTest::qWait(200);
        QCOMPARE(view(), still);
        // A drag.
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
        for (int i = 1; i <= 10; ++i) { QTest::mouseMove(window, at + QPoint(i * 16, i * 9)); QTest::qWait(16); QCOMPARE(view(), still); }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at + QPoint(160, 90));
        QTest::qWait(200);
        QCOMPARE(view(), still);
        QVERIFY(box->x() != before.x() || box->y() != before.y());
        // The map's data refreshes (as it does whenever a VM changes); the view still doesn't move.
        map->setProperty("catalog", QVariant(catalog()));
        QTest::qWait(300);
        QCOMPARE(view(), still);
        // Dragging empty map still pans.
        const QPoint empty = window->contentItem()->mapToScene({30, 300}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, empty);
        for (int i = 1; i <= 6; ++i) { QTest::mouseMove(window, empty + QPoint(i * 10, 0)); QTest::qWait(16); }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, empty + QPoint(60, 0));
        QVERIFY(map->property("panX").toDouble() > panX + 20);
        QVERIFY(QMetaObject::invokeMethod(map, "arrange"));
        QTest::qWait(150);
    }
    // A dropped box snaps to the 40 px grid.
    void snapping() {
        // Map nodes are Repeater delegates, found through the map's own table.
        const auto items = map->property("items").value<QJSValue>();
        auto box = qobject_cast<QQuickItem *>(items.property("vm:22222222-0000-4000-8000-000000000002").toQObject()); QVERIFY(box);
        const QPointF before(box->x(), box->y());
        box->setX(853); box->setY(467);
        QVERIFY(QMetaObject::invokeMethod(map, "remember", Q_ARG(QVariant, "vm:22222222-0000-4000-8000-000000000002"), Q_ARG(QVariant, QVariant::fromValue<QObject *>(box))));
        QCOMPARE(box->x(), 840.0); QCOMPARE(box->y(), 480.0);
        QVERIFY(QMetaObject::invokeMethod(map, "arrange"));
        QJSValueIterator it(items); int nodes = 0;
        while (it.hasNext()) {
            it.next(); auto node = qobject_cast<QQuickItem *>(it.value().toQObject()); if (!node) continue;
            QVERIFY2(qRound(node->x()) % 40 == 0 && qRound(node->y()) % 40 == 0, qPrintable(it.name())); ++nodes;
        }
        QCOMPARE(nodes, 8);
        QVERIFY(shot("map-arranged"));
        Q_UNUSED(before);
    }
    // Each cable gets its own adapter's traffic: by tap name, else by adapter order.
    void perCableTraffic() {
        const auto graph = map->property("graph").toMap(), cables = graph["cableById"].toMap();
        auto stats = [&](const QString &id) { QVariant r; QMetaObject::invokeMethod(map, "nicStats", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, cables[id])); return r.toMap(); };
        const auto web = stats("11111111-0000-4000-8000-000000000001/52:54:00:00:00:01");
        QCOMPARE(web["name"].toString(), QString("vnet1")); QCOMPARE(web["rx"].toDouble(), 1e6); QCOMPARE(web["tx"].toDouble(), 2e5);
        QCOMPARE(stats("33333333-0000-4000-8000-000000000003/52:54:00:00:00:03")["name"].toString(), QString("vnet3"));
        // The pulled cable on FLARE carries nothing and shows no traffic.
        QVariant busy; QMetaObject::invokeMethod(map, "busyness", Q_RETURN_ARG(QVariant, busy), Q_ARG(QVariant, stats("33333333-0000-4000-8000-000000000003/52:54:00:00:00:04")));
        QCOMPARE(busy.toDouble(), 0.0);
        QVariant text; QMetaObject::invokeMethod(map, "rateText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, 1.5e6)); QCOMPARE(text.toString(), QString("1.5 MB/s"));
    }
    // Selecting REMnux lights up what it can reach: its isolated network and FLARE on it, nothing else.
    void reachFocus() {
        map->setProperty("selected", "vm:22222222-0000-4000-8000-000000000002");
        const auto focus = map->property("reachFocus").toMap(), nodes = focus["nodes"].toMap();
        QVERIFY(nodes.contains("bridge:omaiso0002")); QVERIFY(nodes.contains("vm:33333333-0000-4000-8000-000000000003"));
        QVERIFY(!nodes.contains("host")); QVERIFY(!nodes.contains("internet")); QVERIFY(!nodes.contains("vm:11111111-0000-4000-8000-000000000001"));
        QVERIFY(focus["uplinks"].toMap().isEmpty());
        QVERIFY(shot("map-focus-remnux"));
        // The web server reaches the internet through its NAT network and this computer.
        map->setProperty("selected", "vm:11111111-0000-4000-8000-000000000001");
        const auto web = map->property("reachFocus").toMap();
        QVERIFY(web["nodes"].toMap().contains("internet")); QVERIFY(web["uplinks"].toMap().contains("host>internet"));
        QVERIFY(!web["nodes"].toMap().contains("vm:44444444-0000-4000-8000-000000000004")); // stopped
        map->setProperty("selected", "");
        QVERIFY(map->property("reachFocus").isNull() || !map->property("reachFocus").isValid() || map->property("reachFocus").toMap().isEmpty());
    }
    void exports() {
        QVariant text; QVERIFY(QMetaObject::invokeMethod(map, "mermaid", Q_RETURN_ARG(QVariant, text)));
        const auto mermaid = text.toString();
        QVERIFY(mermaid.startsWith("flowchart TB\n"));
        for (const auto &label : {"Internet", "This computer", "lab-nat", "analysis", "web", "remnux", "flare", "10.20.0.10", "pulled"}) QVERIFY2(mermaid.contains(label), label);
        QVERIFY(mermaid.contains(" -.- ")); // FLARE's pulled cable is dotted
        QVERIFY(QMetaObject::invokeMethod(map, "svg", Q_RETURN_ARG(QVariant, text)));
        QDomDocument doc; QVERIFY2(doc.setContent(text.toString()), "SVG must be well-formed XML");
        QCOMPARE(doc.documentElement().tagName(), QString("svg"));
        QCOMPARE(doc.elementsByTagName("polyline").size(), 7);
        QVERIFY(text.toString().contains(">remnux<")); QVERIFY(text.toString().contains("ISOLATED · VMS ONLY"));
        // Saving goes through the same helper the app uses.
        const auto file = dir.filePath("map.svg");
        QVERIFY(QMetaObject::invokeMethod(map->findChild<QObject *>("", Qt::FindDirectChildrenOnly) ? map : map, "exportAs", Q_ARG(QVariant, "svg")));
        QVERIFY(QMetaObject::invokeMethod(map, "saveExport", Q_ARG(QVariant, QUrl::fromLocalFile(file).toString())));
        QVERIFY(QFile(file).size() > 1000);
    }
    void operationsStyleAndMinimap() {
        QVERIFY(QMetaObject::invokeMethod(map, "setOperations", Q_ARG(QVariant, true)));
        QCOMPARE(preferences.values["topologyStyle"].toString(), QString("operations"));
        QVERIFY(shot("map-operations"));
        // Zoomed in, the map no longer fits and the minimap appears.
        QVERIFY(QMetaObject::invokeMethod(map, "zoomAt", Q_ARG(QVariant, 500), Q_ARG(QVariant, 500), Q_ARG(QVariant, 1.9)));
        auto minimap = root->findChild<QObject *>("topologyMinimap"); QVERIFY(minimap);
        QTRY_VERIFY(minimap->property("visible").toBool());
        QVERIFY(shot("map-zoomed-minimap"));
        QVERIFY(QMetaObject::invokeMethod(map, "setOperations", Q_ARG(QVariant, false)));
        QVERIFY(QMetaObject::invokeMethod(map, "refit"));
        QTRY_VERIFY(!minimap->property("visible").toBool());
    }
    // The ping walkthrough follows the configured path and stops, with a reason, where it would be blocked.
    void pingWalkthrough() {
        auto trace = [&](const QString &from, const QString &to) { QVariant r; QMetaObject::invokeMethod(map, "tracePing", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, from), Q_ARG(QVariant, to)); return r.value<QJSValue>().toVariant().toMap(); };
        const QString web = "vm:11111111-0000-4000-8000-000000000001", remnux = "vm:22222222-0000-4000-8000-000000000002", flare = "vm:33333333-0000-4000-8000-000000000003", old = "vm:44444444-0000-4000-8000-000000000004";
        auto online = trace(web, "internet");
        QVERIFY(online["reached"].toBool());
        QStringList where; for (const auto &st : online["steps"].toList()) where << st.toMap()["node"].toString();
        QCOMPARE(where, (QStringList{web, "bridge:omanat0001", "host", "internet", "internet"}));
        QVERIFY(online["steps"].toList()[2].toMap()["text"].toString().contains("NAT"));
        auto sealed = trace(remnux, "internet");
        QVERIFY(!sealed["reached"].toBool());
        QVERIFY(sealed["steps"].toList().last().toMap()["blocked"].toBool());
        QVERIFY(sealed["steps"].toList().last().toMap()["text"].toString().contains("VMs-only"));
        auto lab = trace(remnux, flare);
        QVERIFY(lab["reached"].toBool()); QCOMPARE(lab["steps"].toList()[2].toMap()["node"].toString(), flare);
        auto apart = trace(remnux, web);
        QVERIFY(!apart["reached"].toBool()); QVERIFY(apart["steps"].toList().last().toMap()["text"].toString().contains("No network connects them"));
        QVERIFY(!trace(old, "internet")["reached"].toBool());
        // Walk through it on the map.
        QVERIFY(QMetaObject::invokeMethod(map, "startTrace", Q_ARG(QVariant, web), Q_ARG(QVariant, "internet")));
        auto panel = root->findChild<QObject *>("pingTrace"); QVERIFY(panel); QTRY_VERIFY(panel->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(map, "stepTrace", Q_ARG(QVariant, 2)));
        QTest::qWait(1100);
        QVERIFY(shot("map-ping-step3"));
        QVERIFY(QMetaObject::invokeMethod(map, "startTrace", Q_ARG(QVariant, remnux), Q_ARG(QVariant, "internet")));
        QVERIFY(QMetaObject::invokeMethod(map, "stepTrace", Q_ARG(QVariant, 5)));
        QTest::qWait(1100);
        QVERIFY(shot("map-ping-blocked"));
        map->setProperty("trace", QVariant());
        QTRY_VERIFY(!panel->property("visible").toBool());
    }
    void hoverCard() {
        map->setProperty("hoverCable", "11111111-0000-4000-8000-000000000001/52:54:00:00:00:01");
        map->setProperty("hoverAt", QPointF(400, 500));
        auto card = root->findChild<QObject *>("cableTrafficCard"); QVERIFY(card); QTRY_VERIFY(card->property("visible").toBool());
        QVERIFY(shot("map-hover-card"));
        map->setProperty("hoverCable", "");
    }
    // The map and the theme picker in every built-in theme (screenshots with OMAWARE_SCREENSHOT_DIR).
    void everyTheme() {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\nimport \"" + QUrl::fromLocalFile(QString(OMAWARE_SOURCE_DIR) + "/qml").toString().toUtf8() + "\" as Ui\n"
            "ApplicationWindow { width: 420; height: 980; visible: true; color: theme.colors.raised\n"
            "  palette.windowText: theme.colors.foreground; palette.text: theme.colors.foreground\n"
            "  Ui.ThemePicker { objectName: \"picker\"; x: 16; y: 16; width: 388 } }", QUrl());
        std::unique_ptr<QObject> pickerWindow(component.create()); QVERIFY2(pickerWindow, qPrintable(component.errorString()));
        auto pickerView = qobject_cast<QQuickWindow *>(pickerWindow.get());
        for (const auto &key : Theme::keys()) {
            if (key == "omarchy") continue;
            theme->setMode(key);
            QTest::qWait(120);
            QVERIFY(shot("theme-" + key));
            std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
                if (item->objectName() == "theme_" + key) return item;
                for (auto child : item->childItems()) if (auto hit = find(child)) return hit;
                return nullptr;
            };
            auto card = find(pickerView->contentItem()); QVERIFY(card);
            QVERIFY(card->property("current").toBool());
        }
        const auto out = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR");
        if (!out.isEmpty()) QVERIFY(pickerView->grabWindow().save(out + "/theme-picker.png"));
        theme->setMode("dark");
    }
    // Help shows both guides as searchable topics, and links and sub-headings land on the right one.
    void helpCenter() {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\nimport \"" + QUrl::fromLocalFile(QString(OMAWARE_SOURCE_DIR) + "/qml").toString().toUtf8() + "\" as Ui\n"
            "ApplicationWindow { width: 1180; height: 860; visible: true; color: theme.colors.background\n"
            "  palette.windowText: theme.colors.foreground; palette.text: theme.colors.foreground; palette.base: theme.colors.field\n"
            "  Ui.HelpCenter { objectName: \"help\"; guide: preferences } }", QUrl());
        std::unique_ptr<QObject> win(component.create()); QVERIFY2(win, qPrintable(component.errorString()));
        auto help = win->findChild<QObject *>("help"); QVERIFY(help);
        auto view = qobject_cast<QQuickWindow *>(win.get());
        const auto topics = help->property("topics").value<QJSValue>();
        QVERIFY(topics.property("length").toInt() >= 20);
        auto showTopic = [&](const QString &id) { QMetaObject::invokeMethod(help, "show", Q_ARG(QVariant, id)); return help->property("current").toString(); };
        QCOMPARE(showTopic("snapshots"), QString("snapshots"));
        QCOMPARE(showTopic("taking-a-snapshot"), QString("snapshots"));                 // a ### heading inside it
        QCOMPARE(showTopic("networks/containment"), QString("networks/containment"));
        QTRY_VERIFY(help->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(help, "follow", Q_ARG(QVariant, "#limits")));          // same guide
        QCOMPARE(help->property("current").toString(), QString("networks/limits"));
        QVERIFY(QMetaObject::invokeMethod(help, "follow", Q_ARG(QVariant, "USER-GUIDE.md#the-os-shop")));
        QCOMPARE(help->property("current").toString(), QString("the-os-shop"));
        help->setProperty("query", "bridge permission");
        const int matches = help->property("shown").value<QJSValue>().property("length").toInt();
        QVERIFY(matches > 0 && matches < topics.property("length").toInt());
        help->setProperty("query", "");
        showTopic("the-network-map");
        QTest::qWait(250);
        const auto out = qEnvironmentVariable("OMAWARE_SCREENSHOT_DIR");
        if (!out.isEmpty()) QVERIFY(view->grabWindow().save(out + "/help-network-map.png"));
        help->setProperty("query", "snapshot");
        QTest::qWait(250);
        if (!out.isEmpty()) QVERIFY(view->grabWindow().save(out + "/help-search.png"));
    }
    // Three VMs on their own private internet connections, no networks: one tidy row under This computer.
    void privateConnectionsOnly() {
        auto vm = [](const QString &uuid, const QString &name, const QString &mac) {
            const QVariantList nics{nic(mac, "user", true, "")};
            return QVariantMap{{"uuid", uuid}, {"name", name}, {"active", true}, {"interfaces", nics}, {"liveInterfaces", nics}, {"revision", "r"}};
        };
        showCatalog({{"choices", QVariantList{}}, {"items", QVariantList{}}, {"topology", QVariantList{
            vm("11111111-0000-4000-8000-000000000001", "omaware-web", "52:54:00:00:00:01"),
            vm("22222222-0000-4000-8000-000000000002", "omaware-remnux", "52:54:00:00:00:02"),
            vm("33333333-0000-4000-8000-000000000003", "omaware-flare", "52:54:00:00:00:03")}}});
        QCOMPARE(checkRoutes(), 4);
        // This computer is centred over its VMs.
        const auto items = map->property("items").value<QJSValue>();
        auto x = [&](const QString &id) { auto it = qobject_cast<QQuickItem *>(items.property(id).toQObject()); return it ? it->x() + it->width() / 2 : -1e9; };
        QCOMPARE(x("host"), x("vm:22222222-0000-4000-8000-000000000002"));
        QCOMPARE(x("internet"), x("host"));
        QVERIFY(shot("map-private-only"));
    }
    // A crowded network: VMs stacked in rows climb past each other in lanes, never through a VM.
    void crowdedNetwork() {
        QVariantList topology;
        for (int i = 1; i <= 7; ++i) {
            const auto mac = QString("52:54:00:00:01:%1").arg(i, 2, 10, QChar('0'));
            const QVariantList nics{nic(mac, "bridge:omanat0001", i != 6, QString("vnet%1").arg(10 + i))};
            topology << QVariantMap{{"uuid", QString("aaaaaaaa-0000-4000-8000-%1").arg(i, 12, 10, QChar('0'))}, {"name", QString("omaware-node%1").arg(i)}, {"active", i % 3 != 0},
                {"interfaces", nics}, {"liveInterfaces", i % 3 != 0 ? nics : QVariantList{}}, {"revision", "r"},
                {"addresses", QVariantMap{{mac, QStringList{QString("10.20.0.%1").arg(20 + i)}}}}};
        }
        auto data = catalog(); data["topology"] = topology;
        showCatalog(data);
        QCOMPARE(checkRoutes(), 9);
        QVERIFY(shot("map-crowded"));
        // Dragged somewhere odd, routes still keep clear of every device.
        const auto items = map->property("items").value<QJSValue>();
        auto box = qobject_cast<QQuickItem *>(items.property("bridge:omanat0001").toQObject()); QVERIFY(box);
        box->setX(box->x() + 360); box->setY(box->y() + 40);
        QTest::qWait(100);
        QVERIFY(checkRoutes() > 0);
        QVERIFY(shot("map-crowded-moved"));
    }
};
QTEST_MAIN(TopologyTest)
#include "test_topology.moc"
