// v0.7 regression (live bug): clicking a favorite GIF sent the FIRST
// TRENDING item — the picker resolved the clicked row against gif.results
// instead of the model the user was looking at. This suite drives the real
// GifPicker.qml with the real GifSearchController/favorites models and
// proves the chosen record is the exact provider-qualified favorite, with
// no dependence on (or substitution from) the browse results model.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifResultModel.h"
#include "gif/GifSearchController.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

QVariantMap favoriteFixture(const QString &provider, const QString &id)
{
    // The validated https provider-CDN shape GifStoredModel accepts.
    const QString host = provider == QStringLiteral("giphy")
        ? QStringLiteral("media.giphy.com")
        : QStringLiteral("static.klipy.com");
    QVariantMap m;
    m.insert(QStringLiteral("provider"), provider);
    m.insert(QStringLiteral("gifId"), id);
    m.insert(QStringLiteral("title"), QStringLiteral("fixture %1").arg(id));
    m.insert(QStringLiteral("gifUrl"),
             QStringLiteral("https://%1/%2/original.gif").arg(host, id));
    m.insert(QStringLiteral("previewUrl"),
             QStringLiteral("https://%1/%2/preview.gif").arg(host, id));
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}
} // namespace

class GifPickerSelectionQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("gif-picker-selection-test"));
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void favoriteClickChoosesExactFavoriteNotTrending()
    {
        AppController controller(AppController::MockBackend);
        auto *gif = controller.gif();
        QVERIFY(gif != nullptr);

        // Two favorites from different providers, deliberately NOT present
        // in the browse results model (which stays empty — the live bug
        // substituted its first row).
        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("giphy"), QStringLiteral("aaa1"))));
        QVERIFY(gif->toggleFavorite(
            favoriteFixture(QStringLiteral("klipy"), QStringLiteral("bbb2"))));
        QCOMPARE(gif->favorites()->rowCount(), 2);
        QCOMPARE(gif->results()->rowCount(), 0);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("GifPicker"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *picker = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(picker != nullptr);

        QQuickWindow window;
        window.resize(600, 640);
        if (auto *item = qobject_cast<QQuickItem *>(picker))
            item->setParentItem(window.contentItem());
        else if (auto *popupItem =
                     picker->property("contentItem").value<QQuickItem *>())
            popupItem->setParentItem(window.contentItem());
        window.show();
        QCoreApplication::processEvents();

        QSignalSpy chosen(picker, SIGNAL(gifChosen(QVariant)));
        QQmlProperty::write(picker, QStringLiteral("section"),
                            QStringLiteral("favorites"));
        QCOMPARE(QQmlProperty::read(picker, QStringLiteral("section"))
                     .toString(),
                 QStringLiteral("favorites"));

        // Click row 1 of the VISIBLE favorites grid (the klipy favorite —
        // favorites prepend newest first, so verify by identity, not
        // position assumptions).
        const QVariantMap expected = gif->favorites()->get(1);
        QVERIFY(!expected.value(QStringLiteral("gifId")).toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(picker, "choose",
                                          Q_ARG(QVariant, 1)));

        QCOMPARE(chosen.count(), 1);
        const QVariantMap result = chosen.at(0).at(0).toMap();
        QCOMPARE(result.value(QStringLiteral("provider")).toString(),
                 expected.value(QStringLiteral("provider")).toString());
        QCOMPARE(result.value(QStringLiteral("gifId")).toString(),
                 expected.value(QStringLiteral("gifId")).toString());
        QCOMPARE(result.value(QStringLiteral("gifUrl")).toString(),
                 expected.value(QStringLiteral("gifUrl")).toString());
        // And the identity is provider-qualified — never a bare id that
        // could collide across GIPHY and KLIPY.
        QVERIFY(!result.value(QStringLiteral("provider")).toString().isEmpty());

        // An out-of-range or unidentifiable row chooses NOTHING (no
        // index-zero fallback, no substitution).
        QVERIFY(QMetaObject::invokeMethod(picker, "choose",
                                          Q_ARG(QVariant, 99)));
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(warnings, QStringList{});
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(GifPickerSelectionQmlTest)
#include "GifPickerSelectionQmlTest.moc"
