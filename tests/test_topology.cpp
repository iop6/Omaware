// SPDX-License-Identifier: GPL-3.0-or-later
// The actual network map QML with an inert backend and a fixed lab: no libvirt, no host changes.
#include <QtTest>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include "theme.h"

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
    // Every cable and uplink is made of horizontal and vertical runs only.
    void squareCables() {
        const auto graph = map->property("graph").toMap();
        int checked = 0;
        auto axisAligned = [&](const QVariant &geometry) {
            const auto pts = geometry.toMap()["pts"].toList();
            if (pts.size() < 2) return false;
            for (int i = 1; i < pts.size(); ++i) {
                const auto a = pts[i - 1].toMap(), b = pts[i].toMap();
                if (qAbs(a["x"].toDouble() - b["x"].toDouble()) > .01 && qAbs(a["y"].toDouble() - b["y"].toDouble()) > .01) return false;
            }
            return true;
        };
        for (const auto &c : graph["cables"].toList()) {
            QVariant g; QVERIFY(QMetaObject::invokeMethod(map, "geometry", Q_RETURN_ARG(QVariant, g), Q_ARG(QVariant, c)));
            QVERIFY2(axisAligned(g), qPrintable(c.toMap()["id"].toString())); ++checked;
        }
        for (const auto &u : graph["uplinks"].toList()) {
            QVariant g; QVERIFY(QMetaObject::invokeMethod(map, "uplinkGeometry", Q_RETURN_ARG(QVariant, g), Q_ARG(QVariant, u)));
            QVERIFY2(axisAligned(g), qPrintable(u.toMap()["id"].toString())); ++checked;
        }
        QCOMPARE(checked, 7);
        QVERIFY(shot("map-overview"));
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
    void hoverCard() {
        map->setProperty("hoverCable", "11111111-0000-4000-8000-000000000001/52:54:00:00:00:01");
        map->setProperty("hoverAt", QPointF(400, 500));
        auto card = root->findChild<QObject *>("cableTrafficCard"); QVERIFY(card); QTRY_VERIFY(card->property("visible").toBool());
        QVERIFY(shot("map-hover-card"));
        map->setProperty("hoverCable", "");
    }
};
QTEST_MAIN(TopologyTest)
#include "test_topology.moc"
