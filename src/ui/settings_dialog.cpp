#include "settings_dialog.h"

#include "backup_service.h"
#include "localization.h"

#include <oclero/qlementine/widgets/Switch.hpp>
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>

#include <windows.h>

#include <QtConcurrent>

namespace {

class SettingsCard final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(palette().color(QPalette::Mid));
        painter.setBrush(palette().color(QPalette::Base));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    }
};

class SettingsComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

    void showPopup() override {
        QComboBox::showPopup();
        auto *popup = view()->parentWidget();
        if (!popup->isVisible() || !screen()) return;
        // Qt 默认把当前项对齐到控件上；设置页始终优先从控件下缘展开。
        const QRect available = screen()->availableGeometry();
        const QPoint below = mapToGlobal(QPoint(0, height() + 4));
        const int preferredY = below.y() + popup->height() <= available.bottom() + 1
            ? below.y() : mapToGlobal(QPoint(0, -popup->height() - 4)).y();
        popup->move(qBound(available.left(), below.x(), qMax(available.left(), available.right() - popup->width() + 1)),
                    qBound(available.top(), preferredY, qMax(available.top(), available.bottom() - popup->height() + 1)));
    }
};

class SettingsNavigationDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {48, 40}; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(QApplication::style());
        const QPalette colors = style ? style->theme().palette : option.palette;
        const QRect row = option.rect.adjusted(4, 2, -4, -2);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        if (selected || option.state.testFlag(QStyle::State_MouseOver)) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.color(QPalette::AlternateBase));
            painter->drawRoundedRect(row, 4, 4);
        }
        if (selected) {
            painter->setBrush(colors.color(QPalette::Highlight));
            painter->drawRoundedRect(QRectF(row.left(), row.top() + 10, 3, 16), 1.5, 1.5);
        }
        QFont icons;
        icons.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
        icons.setPixelSize(16);
        painter->setFont(icons);
        painter->setPen(colors.color(QPalette::WindowText));
        painter->drawText(QRect(row.left() + 8, row.top(), 24, row.height()), Qt::AlignCenter,
                          QString(QChar(index.data(Qt::UserRole).toUInt())));
        if (row.width() > 80) {
            painter->setFont(option.font);
            const QRect label = row.adjusted(40, 0, -8, 0);
            painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(option.font).elidedText(index.data().toString(), Qt::ElideRight, label.width()));
        }
        painter->restore();
    }
};

class SettingsComboDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {0, 34}; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QPalette colors = option.palette;
        const QRect row = option.rect.adjusted(4, 2, -4, -2);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        if (selected || option.state.testFlag(QStyle::State_MouseOver)) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.color(QPalette::AlternateBase));
            painter->drawRoundedRect(row, 5, 5);
        }
        if (selected) {
            painter->setBrush(colors.color(QPalette::Highlight));
            painter->drawRoundedRect(QRectF(row.left() + 4, row.top() + 7, 3, row.height() - 14), 1.5, 1.5);
        }
        const QRect label = row.adjusted(16, 0, -10, 0);
        painter->setFont(option.font);
        painter->setPen(colors.color(option.state.testFlag(QStyle::State_Enabled)
            ? QPalette::Text : QPalette::PlaceholderText));
        painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(option.font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, label.width()));
        painter->restore();
    }
};

QWidget *pageBody(QWidget *page) {
    if (auto *area = qobject_cast<QScrollArea *>(page)) return area->widget();
    return page;
}

QVBoxLayout *pageLayout(QWidget *page) {
    return qobject_cast<QVBoxLayout *>(pageBody(page)->layout());
}

QString shortcutText(QKeyEvent *event) {
    QString value = QKeySequence(event->modifiers() | event->key()).toString(QKeySequence::PortableText);
    value.replace(QStringLiteral("Return"), QStringLiteral("Enter"), Qt::CaseInsensitive);
    value.replace(QLatin1Char('+'), QStringLiteral(" + "));
    return value;
}

class ShortcutButton final : public QPushButton {
    Q_OBJECT
public:
    explicit ShortcutButton(bool needsModifier, QWidget *parent = nullptr)
        : QPushButton(parent), needsModifier_(needsModifier) {}

    void beginCapture() {
        previous_ = text();
        capturing_ = true;
        setText(AppLocalization::get(QStringLiteral("ShortcutPlaceholder")));
        setFocus(Qt::ShortcutFocusReason);
    }

signals:
    void valueCaptured(const QString &value);

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (!capturing_) { QPushButton::keyPressEvent(event); return; }
        if (event->key() == Qt::Key_Escape) {
            capturing_ = false;
            setText(previous_);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Control || event->key() == Qt::Key_Alt
            || event->key() == Qt::Key_Shift || event->key() == Qt::Key_Meta) return;
        if (needsModifier_ && event->modifiers() == Qt::NoModifier) return;
        const QString value = shortcutText(event);
        if (value.isEmpty()) return;
        capturing_ = false;
        setText(value);
        emit valueCaptured(value);
        event->accept();
    }

private:
    friend class SettingsDialog;
    bool needsModifier_ = false;
    bool capturing_ = false;
    QString previous_;
};

QString themeLabel(const QString &mode) {
    if (mode == QStringLiteral("Light")) return AppLocalization::get(QStringLiteral("ThemeLightOptionContent"));
    if (mode == QStringLiteral("Dark")) return AppLocalization::get(QStringLiteral("ThemeDarkOptionContent"));
    return AppLocalization::get(QStringLiteral("ThemeSystemOptionContent"));
}

QString languageLabel(const QString &language) {
    if (language == QStringLiteral("zh-CN")) return QStringLiteral("简体中文");
    if (language == QStringLiteral("en-US")) return QStringLiteral("English");
    return AppLocalization::get(QStringLiteral("LanguageSystemOptionContent"));
}

QStyle *comboPopupStyle() {
    static QStyle *style = [] {
        auto *fusion = QStyleFactory::create(QStringLiteral("Fusion"));
        fusion->setParent(qApp);
        return fusion;
    }();
    return style;
}
}

SettingsDialog::SettingsDialog(AppSettings settings, QString databasePath, QString settingsPath, QWidget *parent)
    : QDialog(parent), settings_(std::move(settings)), databasePath_(std::move(databasePath)),
      settingsPath_(std::move(settingsPath)) {
    setWindowTitle(AppLocalization::get(QStringLiteral("SettingsWindowTitle")));
    setWindowIcon(QIcon(QStringLiteral(":/PasteOrbit.ico")));
    setAutoFillBackground(true);
    resize(760, 680);
    setMinimumSize(600, 560);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    auto *body = new QHBoxLayout;
    body->setSpacing(8);
    navigation_ = new QListWidget(this);
    navigation_->setObjectName(QStringLiteral("SettingsNavigation"));
    navigation_->setFixedWidth(200);
    navigation_->setMouseTracking(true);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setItemDelegate(new SettingsNavigationDelegate(navigation_));
    tabs_ = new QStackedWidget(this);
    body->addWidget(navigation_);
    body->addWidget(tabs_, 1);
    outer->addLayout(body, 1);
    connect(navigation_, &QListWidget::currentRowChanged, tabs_, &QStackedWidget::setCurrentIndex);
    const auto addTab = [this](QWidget *page, const QString &key) {
        page->setProperty("TabLocalizationKey", key);
        const uint glyphs[]{0xE713, 0xE765, 0xE823, 0xE8A5, 0xE946};
        auto *item = new QListWidgetItem(AppLocalization::get(key), navigation_);
        item->setData(Qt::UserRole, glyphs[tabs_->count()]);
        item->setToolTip(item->text());
        tabs_->addWidget(page);
    };

    auto *general = createPage(QStringLiteral("SettingsStartupSectionText"));
    startup_ = new oclero::qlementine::Switch(general);
    administrator_ = new oclero::qlementine::Switch(general);
    autoHide_ = new oclero::qlementine::Switch(general);
    addCard(general, QStringLiteral("SettingsStartupCardHeader"), QStringLiteral("SettingsStartupCardDescription"), startup_);
    addCard(general, QStringLiteral("SettingsRunAsAdministratorCardHeader"), QStringLiteral("SettingsRunAsAdministratorCardDescription"), administrator_);
    addCard(general, QStringLiteral("SettingsAutoHideCardHeader"), QStringLiteral("SettingsAutoHideCardDescription"), autoHide_);
    auto *monitorHeading = new QLabel(AppLocalization::get(QStringLiteral("SettingsMonitoringSectionText")), general);
    monitorHeading->setObjectName(QStringLiteral("SectionHeading"));
    pageLayout(general)->addWidget(monitorHeading);
    monitorText_ = new oclero::qlementine::Switch(general);
    monitorImages_ = new oclero::qlementine::Switch(general);
    monitorFiles_ = new oclero::qlementine::Switch(general);
    addCard(general, QStringLiteral("SettingsMonitorTextCardHeader"), QStringLiteral("SettingsMonitorTextCardDescription"), monitorText_);
    addCard(general, QStringLiteral("SettingsMonitorImagesCardHeader"), QStringLiteral("SettingsMonitorImagesCardDescription"), monitorImages_);
    addCard(general, QStringLiteral("SettingsMonitorFilesCardHeader"), QStringLiteral("SettingsMonitorFilesCardDescription"), monitorFiles_);
    auto *appearanceHeading = new QLabel(AppLocalization::get(QStringLiteral("SettingsAppearanceSectionText")), general);
    appearanceHeading->setObjectName(QStringLiteral("SectionHeading"));
    pageLayout(general)->addWidget(appearanceHeading);
    theme_ = new SettingsComboBox(general);
    theme_->addItem(themeLabel(QStringLiteral("System")), QStringLiteral("System"));
    theme_->addItem(themeLabel(QStringLiteral("Light")), QStringLiteral("Light"));
    theme_->addItem(themeLabel(QStringLiteral("Dark")), QStringLiteral("Dark"));
    language_ = new SettingsComboBox(general);
    language_->addItem(languageLabel({}), QString{});
    language_->addItem(languageLabel(QStringLiteral("zh-CN")), QStringLiteral("zh-CN"));
    language_->addItem(languageLabel(QStringLiteral("en-US")), QStringLiteral("en-US"));
    addCard(general, QStringLiteral("SettingsThemeCardHeader"), QStringLiteral("SettingsThemeCardDescription"), theme_);
    addCard(general, QStringLiteral("SettingsLanguageCardHeader"), QStringLiteral("SettingsLanguageCardDescription"), language_);
    addTab(general, QStringLiteral("SettingsGeneralNavigationContent"));

    auto *hotkeys = createPage(QStringLiteral("SettingsHotKeySectionText"));
    interceptWinV_ = new oclero::qlementine::Switch(hotkeys);
    addCard(hotkeys, QStringLiteral("SettingsInterceptWinVCardHeader"), QStringLiteral("SettingsInterceptWinVCardDescription"), interceptWinV_);
    const struct { const char *name; const char *description; const char *key; bool global; } shortcutDefs[]{
        {"SettingsGlobalHotKeyCardHeader", "SettingsGlobalHotKeyCardDescription", "GlobalHotKey", true},
        {"SettingsPasteShortcutCardHeader", "", "PasteShortcut", false},
        {"SettingsPlainTextShortcutCardHeader", "", "PlainTextPasteShortcut", false},
        {"SettingsPreviewShortcutCardHeader", "", "PreviewShortcut", false},
        {"SettingsPinShortcutCardHeader", "", "PinShortcut", false},
        {"SettingsPasteAsFileShortcutCardHeader", "", "PasteAsFileShortcut", false}};
    for (const auto &definition : shortcutDefs) {
        auto *button = new ShortcutButton(definition.global, hotkeys);
        button->setObjectName(QString::fromLatin1(definition.key));
        shortcuts_.insert(QString::fromLatin1(definition.key), button);
        addCard(hotkeys, QString::fromLatin1(definition.name), QString::fromLatin1(definition.description), button);
        connect(button, &QPushButton::clicked, button, [button] {
            button->beginCapture();
        });
        connect(button, &ShortcutButton::valueCaptured, this, [this, key = QString::fromLatin1(definition.key)](const QString &value) {
            if (key == QStringLiteral("GlobalHotKey")) settings_.globalHotKey = value;
            else if (key == QStringLiteral("PasteShortcut")) settings_.pasteShortcut = value;
            else if (key == QStringLiteral("PlainTextPasteShortcut")) settings_.plainTextPasteShortcut = value;
            else if (key == QStringLiteral("PreviewShortcut")) settings_.previewShortcut = value;
            else if (key == QStringLiteral("PinShortcut")) settings_.pinShortcut = value;
            else settings_.pasteAsFileShortcut = value;
            persist();
        });
    }
    auto *shortcutHint = new QLabel(AppLocalization::get(QStringLiteral("SettingsShortcutHintText")), hotkeys);
    shortcutHint->setWordWrap(true);
    shortcutHint->setObjectName(QStringLiteral("MutedText"));
    shortcutHint->setProperty("LocalizationKey", QStringLiteral("SettingsShortcutHintText"));
    pageLayout(hotkeys)->addWidget(shortcutHint);
    addTab(hotkeys, QStringLiteral("SettingsHotKeyNavigationContent"));

    auto *history = createPage(QStringLiteral("SettingsHistorySectionText"));
    retention_ = new SettingsComboBox(history);
    for (const int days : {7, 30, 90, 365}) retention_->addItem(QString::number(days), days);
    maximumEntries_ = new SettingsComboBox(history);
    for (const int count : {1000, 5000, 10000}) maximumEntries_->addItem(QString::number(count), count);
    addCard(history, QStringLiteral("SettingsRetentionCardHeader"), QStringLiteral("SettingsRetentionCardDescription"), retention_);
    addCard(history, QStringLiteral("SettingsMaxEntriesCardHeader"), QStringLiteral("SettingsMaxEntriesCardDescription"), maximumEntries_);
    auto *pinnedHint = new QLabel(AppLocalization::get(QStringLiteral("SettingsPinnedRetentionHintText")), history);
    pinnedHint->setWordWrap(true);
    pinnedHint->setObjectName(QStringLiteral("MutedText"));
    pageLayout(history)->addWidget(pinnedHint);
    auto *defaults = new QPushButton(AppLocalization::get(QStringLiteral("RestoreDefaultsButtonContent")), history);
    defaults->setProperty("LocalizationKey", QStringLiteral("RestoreDefaultsButtonContent"));
    connect(defaults, &QPushButton::clicked, this, &SettingsDialog::restoreDefaults);
    pageLayout(history)->addWidget(defaults, 0, Qt::AlignRight);
    addTab(history, QStringLiteral("SettingsHistoryNavigationContent"));

    auto *privacy = createPage(QStringLiteral("SettingsPrivacySectionText"));
    auto *excludedRow = new QWidget(privacy);
    auto *excludedLayout = new QHBoxLayout(excludedRow);
    excludedLayout->setContentsMargins(0, 0, 0, 0);
    excludedApplications_ = new QLineEdit(excludedRow);
    excludedApplications_->setPlaceholderText(AppLocalization::get(QStringLiteral("ExcludedApplicationsTextBoxPlaceholder")));
    auto *processButton = new QPushButton(AppLocalization::get(QStringLiteral("SelectProcessButtonContent")), excludedRow);
    processButton->setProperty("LocalizationKey", QStringLiteral("SelectProcessButtonContent"));
    excludedLayout->addWidget(excludedApplications_, 1);
    excludedLayout->addWidget(processButton);
    addCard(privacy, QStringLiteral("SettingsExcludedAppsCardHeader"), QStringLiteral("SettingsExcludedAppsCardDescription"), excludedRow);
    connect(processButton, &QPushButton::clicked, this, &SettingsDialog::chooseProcesses);
    auto *backupRow = new QWidget(privacy);
    auto *backupLayout = new QHBoxLayout(backupRow);
    backupLayout->setContentsMargins(0, 0, 0, 0);
    auto *exportButton = new QPushButton(AppLocalization::get(QStringLiteral("ExportBackupButtonContent")), backupRow);
    auto *restoreButton = new QPushButton(AppLocalization::get(QStringLiteral("RestoreBackupButtonContent")), backupRow);
    exportButton->setProperty("LocalizationKey", QStringLiteral("ExportBackupButtonContent"));
    restoreButton->setProperty("LocalizationKey", QStringLiteral("RestoreBackupButtonContent"));
    backupLayout->addWidget(exportButton);
    backupLayout->addWidget(restoreButton);
    backupLayout->addStretch(1);
    addCard(privacy, QStringLiteral("SettingsBackupCardHeader"), QStringLiteral("SettingsBackupCardDescription"), backupRow);
    connect(exportButton, &QPushButton::clicked, this, &SettingsDialog::exportBackup);
    connect(restoreButton, &QPushButton::clicked, this, &SettingsDialog::restoreBackup);
    auto *protection = new QLabel(AppLocalization::get(QStringLiteral("SettingsProtectionDescriptionText")), privacy);
    protection->setWordWrap(true);
    protection->setObjectName(QStringLiteral("MutedText"));
    protection->setProperty("LocalizationKey", QStringLiteral("SettingsProtectionDescriptionText"));
    pageLayout(privacy)->addWidget(protection);
    addTab(privacy, QStringLiteral("SettingsPrivacyNavigationContent"));

    auto *about = createPage(QStringLiteral("SettingsAboutSectionText"));
    auto *aboutIcon = new QLabel(about);
    aboutIcon->setAlignment(Qt::AlignCenter);
    aboutIcon->setPixmap(QIcon(QStringLiteral(":/PasteOrbit.png")).pixmap(72, 72));
    pageLayout(about)->addWidget(aboutIcon);
    auto *brand = new QLabel(QStringLiteral("PasteOrbit\n%1").arg(QCoreApplication::applicationVersion()), about);
    brand->setAlignment(Qt::AlignCenter);
    brand->setObjectName(QStringLiteral("AboutBrand"));
    pageLayout(about)->addWidget(brand);
    auto *projectButton = new QPushButton(AppLocalization::get(QStringLiteral("GitHubProjectButtonText")), about);
    auto *updateButton = new QPushButton(AppLocalization::get(QStringLiteral("CheckForUpdatesButtonContent")), about);
    projectButton->setProperty("LocalizationKey", QStringLiteral("GitHubProjectButtonText"));
    updateButton->setProperty("LocalizationKey", QStringLiteral("CheckForUpdatesButtonContent"));
    addCard(about, QStringLiteral("GitHubProjectCardHeader"), QStringLiteral("GitHubProjectCardDescription"), projectButton);
    addCard(about, QStringLiteral("SettingsUpdateCardHeader"), QStringLiteral("SettingsUpdateCardDescription"), updateButton);
    connect(projectButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/ShrlAlgo/PasteOrbit")));
    });
    connect(updateButton, &QPushButton::clicked, this, &SettingsDialog::checkUpdatesRequested);
    addTab(about, QStringLiteral("SettingsAboutNavigationContent"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(AppLocalization::activeLanguage() == QStringLiteral("en-US")
        ? QStringLiteral("Close") : QStringLiteral("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    outer->addWidget(buttons);
    if (auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(QApplication::style()))
        connect(style, &oclero::qlementine::QlementineStyle::themeChanged, this, &SettingsDialog::applyTheme);
    navigation_->setCurrentRow(0);
    loadControls(settings_);

    for (auto *check : {startup_, administrator_, autoHide_, monitorText_, monitorImages_, monitorFiles_, interceptWinV_}) {
        connect(check, &QAbstractButton::toggled, this, [this] { if (!loading_) persist(); });
    }
    for (auto *combo : {theme_, language_, retention_, maximumEntries_}) {
        // 保留原生 ComboBox 键盘交互，弹层与选项各自只绘制一层 Fluent 风格表面。
        auto *view = combo->view();
        auto *popup = view->parentWidget();
        popup->setStyle(comboPopupStyle());
        view->setStyle(comboPopupStyle());
        popup->setWindowFlag(Qt::FramelessWindowHint);
        popup->setAttribute(Qt::WA_TranslucentBackground);
        popup->setAutoFillBackground(false);
        popup->layout()->setContentsMargins(4, 4, 4, 4);
        view->setFrameShape(QFrame::NoFrame);
        view->setMouseTracking(true);
        view->setStyleSheet(QStringLiteral("QAbstractItemView{background:transparent;border:0;outline:0;}"));
        view->viewport()->setAutoFillBackground(false);
        view->setItemDelegate(new SettingsComboDelegate(view));
        combo->setMaxVisibleItems(combo->count());
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) persist(); });
    }
    applyTheme();
    connect(excludedApplications_, &QLineEdit::editingFinished, this, [this] { if (!loading_) persist(); });
}

void SettingsDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (navigation_) navigation_->setFixedWidth(width() < 760 ? 48 : 200);
}

void SettingsDialog::applyTheme() {
    // 样式表中的 palette(...) 不会随主题重算，统一从 Qlementine 当前调色板重新应用。
    const auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(QApplication::style());
    const QPalette palette = style ? style->theme().palette : qApp->palette();
    setPalette(palette);
    // 弹层是独立窗口；仅其表面颜色随主题重绘，选项颜色由调色板直接提供。
    for (auto *combo : {theme_, language_, retention_, maximumEntries_}) {
        combo->setPalette(palette);
        auto *view = combo->view();
        auto *popup = view->parentWidget();
        popup->setPalette(palette);
        view->setPalette(palette);
        view->viewport()->setPalette(palette);
        popup->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;border-radius:8px;")
            .arg(palette.color(QPalette::Base).name(), palette.color(QPalette::Mid).name()));
        view->viewport()->update();
    }
    // 不在对话框祖先上设置样式表，否则 Switch::style() 会变成 QStyleSheetStyle。
    navigation_->setStyleSheet(QStringLiteral("QListWidget#SettingsNavigation{background:%1;color:%2;border:0;outline:0;}")
        .arg(palette.color(QPalette::Window).name(), palette.color(QPalette::WindowText).name()));
    for (auto *label : findChildren<QLabel *>()) {
        const QString name = label->objectName();
        if (name == QStringLiteral("CardTitle"))
            label->setStyleSheet(QStringLiteral("font-weight:600;color:%1;").arg(palette.color(QPalette::WindowText).name()));
        else if (name == QStringLiteral("CardDescription") || name == QStringLiteral("MutedText"))
            label->setStyleSheet(QStringLiteral("color:%1;").arg(palette.color(QPalette::PlaceholderText).name()));
        else if (name == QStringLiteral("SectionHeading"))
            label->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;margin-top:12px;color:%1;")
                .arg(palette.color(QPalette::WindowText).name()));
        else if (name == QStringLiteral("AboutBrand"))
            label->setStyleSheet(QStringLiteral("font-size:22px;font-weight:600;padding:22px;color:%1;")
                .arg(palette.color(QPalette::WindowText).name()));
    }
    // 进程选择结果写入 QLineEdit 后，文字色不能依赖父对话框样式继承。
    excludedApplications_->setStyleSheet(QStringLiteral("color:%1;selection-color:%2;selection-background-color:%3;")
        .arg(palette.color(QPalette::Text).name(), palette.color(QPalette::HighlightedText).name(),
             palette.color(QPalette::Highlight).name()));
    navigation_->viewport()->update();
}

QWidget *SettingsDialog::createPage(const QString &headingKey) {
    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("SettingsPage"));
    content->setBackgroundRole(QPalette::Window);
    content->setAutoFillBackground(true);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    layout->setAlignment(Qt::AlignTop);
    auto *heading = new QLabel(AppLocalization::get(headingKey), content);
    heading->setObjectName(QStringLiteral("SectionHeading"));
    heading->setProperty("LocalizationKey", headingKey);
    layout->addWidget(heading);
    auto *line = new QFrame(content);
    line->setFrameShape(QFrame::HLine);
    layout->addWidget(line);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setBackgroundRole(QPalette::Window);
    scroll->viewport()->setAutoFillBackground(true);
    scroll->setWidget(content);
    return scroll;
}

void SettingsDialog::addCard(QWidget *page, const QString &titleKey, const QString &descriptionKey, QWidget *control) {
    QWidget *content = pageBody(page);
    auto *card = new SettingsCard(content);
    card->setObjectName(QStringLiteral("SettingsCard"));
    card->setMinimumHeight(64);
    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(16, 12, 16, 12);
    cardLayout->setSpacing(16);
    auto *labels = new QVBoxLayout();
    auto *title = new QLabel(AppLocalization::get(titleKey), card);
    title->setObjectName(QStringLiteral("CardTitle"));
    title->setWordWrap(true);
    title->setProperty("LocalizationKey", titleKey);
    labels->addWidget(title);
    if (!descriptionKey.isEmpty()) {
        auto *description = new QLabel(AppLocalization::get(descriptionKey), card);
        description->setObjectName(QStringLiteral("CardDescription"));
        description->setProperty("LocalizationKey", descriptionKey);
        description->setWordWrap(true);
        labels->addWidget(description);
    }
    cardLayout->addLayout(labels, 1);
    if (control->parentWidget() != card) control->setParent(card);
    control->setAccessibleName(title->text());
    control->setProperty("AccessibleLocalizationKey", titleKey);
    if (qobject_cast<QComboBox *>(control)) control->setMinimumWidth(180);
    cardLayout->addWidget(control, 0, Qt::AlignVCenter);
    auto *bodyLayout = qobject_cast<QVBoxLayout *>(content->layout());
    bodyLayout->addWidget(card);
}

void SettingsDialog::loadControls(const AppSettings &settings) {
    loading_ = true;
    startup_->setChecked(settings.startWithWindows);
    administrator_->setChecked(settings.runAsAdministrator);
    autoHide_->setChecked(settings.autoHideOnDeactivate);
    monitorText_->setChecked(settings.monitorText);
    monitorImages_->setChecked(settings.monitorImages);
    monitorFiles_->setChecked(settings.monitorFiles);
    interceptWinV_->setChecked(settings.interceptWindowsClipboardShortcut);
    theme_->setCurrentIndex(theme_->findData(settings.themeMode));
    language_->setCurrentIndex(language_->findData(settings.language));
    // 保留旧版本或手工配置的正数，避免无匹配选项时保存成零。
    if (retention_->findData(settings.retentionDays) < 0)
        retention_->addItem(QString::number(settings.retentionDays), settings.retentionDays);
    if (maximumEntries_->findData(settings.maxHistoryEntries) < 0)
        maximumEntries_->addItem(QString::number(settings.maxHistoryEntries), settings.maxHistoryEntries);
    retention_->setCurrentIndex(retention_->findData(settings.retentionDays));
    maximumEntries_->setCurrentIndex(maximumEntries_->findData(settings.maxHistoryEntries));
    excludedApplications_->setText(settings.excludedApplications);
    const QHash<QString, QString> values{{"GlobalHotKey", settings.globalHotKey}, {"PasteShortcut", settings.pasteShortcut},
        {"PlainTextPasteShortcut", settings.plainTextPasteShortcut}, {"PreviewShortcut", settings.previewShortcut},
        {"PinShortcut", settings.pinShortcut}, {"PasteAsFileShortcut", settings.pasteAsFileShortcut}};
    for (auto it = values.cbegin(); it != values.cend(); ++it) if (shortcuts_.contains(it.key())) shortcuts_[it.key()]->setText(it.value());
    loading_ = false;
}

void SettingsDialog::saveControls() {
    settings_.startWithWindows = startup_->isChecked();
    settings_.runAsAdministrator = administrator_->isChecked();
    settings_.autoHideOnDeactivate = autoHide_->isChecked();
    settings_.monitorText = monitorText_->isChecked();
    settings_.monitorImages = monitorImages_->isChecked();
    settings_.monitorFiles = monitorFiles_->isChecked();
    settings_.interceptWindowsClipboardShortcut = interceptWinV_->isChecked();
    settings_.themeMode = theme_->currentData().toString();
    settings_.language = language_->currentData().toString();
    settings_.retentionDays = retention_->currentData().toInt();
    settings_.maxHistoryEntries = maximumEntries_->currentData().toInt();
    settings_.excludedApplications = excludedApplications_->text().trimmed();
}

void SettingsDialog::persist(bool preserveSkippedVersion) {
    saveControls();
    // 更新服务独立保存跳过版本，设置页的旧副本不能覆盖它。
    if (preserveSkippedVersion)
        settings_.skippedUpdateVersion = AppSettings::load(settingsPath_).skippedUpdateVersion;
    if (!settings_.save(settingsPath_)) {
        QMessageBox::warning(this, AppLocalization::get(QStringLiteral("SettingsSaveFailedTitle")),
                             AppLocalization::get(QStringLiteral("SettingsSaveFailed")));
        return;
    }
    QSettings startup(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                       QSettings::NativeFormat);
    const QString startupCommand = settings_.startWithWindows
        ? QStringLiteral("\"%1\"").arg(QCoreApplication::applicationFilePath()) : QString{};
    if (startup.value(QStringLiteral("PasteOrbit")).toString() != startupCommand) {
        if (startupCommand.isEmpty()) startup.remove(QStringLiteral("PasteOrbit"));
        else startup.setValue(QStringLiteral("PasteOrbit"), startupCommand);
    }
    QString language = settings_.language;
    if (language.isEmpty()) language = QLocale().name().startsWith(QStringLiteral("en"))
        ? QStringLiteral("en-US") : QStringLiteral("zh-CN");
    // 主题切换不重新解析本地化 XML、遍历所有设置控件或写启动项。
    if (language != AppLocalization::activeLanguage()) {
        AppLocalization::load(language);
        refreshLocalization();
    }
    emit settingsChanged(settings_);
}

void SettingsDialog::refreshLocalization() {
    setWindowTitle(AppLocalization::get(QStringLiteral("SettingsWindowTitle")));
    for (auto *label : findChildren<QLabel *>()) {
        const auto key = label->property("LocalizationKey").toString();
        if (!key.isEmpty()) label->setText(AppLocalization::get(key));
    }
    for (auto *button : findChildren<QPushButton *>()) {
        const auto key = button->property("LocalizationKey").toString();
        if (!key.isEmpty()) button->setText(AppLocalization::get(key));
    }
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *item = navigation_->item(i);
        item->setText(AppLocalization::get(tabs_->widget(i)->property("TabLocalizationKey").toString()));
        item->setToolTip(item->text());
    }
    for (auto *widget : findChildren<QWidget *>()) {
        const auto key = widget->property("AccessibleLocalizationKey").toString();
        if (!key.isEmpty()) widget->setAccessibleName(AppLocalization::get(key));
    }
    theme_->setItemText(0, themeLabel(QStringLiteral("System")));
    theme_->setItemText(1, themeLabel(QStringLiteral("Light")));
    theme_->setItemText(2, themeLabel(QStringLiteral("Dark")));
    language_->setItemText(0, languageLabel({}));
    excludedApplications_->setPlaceholderText(AppLocalization::get(QStringLiteral("ExcludedApplicationsTextBoxPlaceholder")));
    if (auto *close = findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close))
        close->setText(AppLocalization::activeLanguage() == QStringLiteral("en-US")
            ? QStringLiteral("Close") : QStringLiteral("关闭"));
}

void SettingsDialog::chooseProcesses() {
    QDialog picker(this);
    picker.setWindowTitle(AppLocalization::get(QStringLiteral("SelectExcludedAppsTitle")));
    picker.resize(440, 520);
    auto *layout = new QVBoxLayout(&picker);
    auto *search = new QLineEdit(&picker);
    search->setPlaceholderText(AppLocalization::get(QStringLiteral("ProcessSearchPlaceholder")));
    layout->addWidget(search);
    auto *list = new QListWidget(&picker);
    layout->addWidget(list, 1);
    QHash<QString, QString> windows;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
        wchar_t title[256]{};
        if (!GetWindowTextW(window, title, 256)) return TRUE;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process) return TRUE;
        wchar_t path[MAX_PATH]{};
        DWORD length = MAX_PATH;
        const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!ok) return TRUE;
        const QString executable = QString::fromWCharArray(path, static_cast<int>(length));
        const QString name = executable.mid(executable.lastIndexOf(QLatin1Char('\\')) + 1);
        const QString display = QStringLiteral("%1 (%2)").arg(QString::fromWCharArray(title), name);
        static_cast<QHash<QString, QString> *>(reinterpret_cast<void *>(parameter))->insert(name, display);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    QStringList names = windows.keys();
    names.sort(Qt::CaseInsensitive);
    QString configured = excludedApplications_->text();
    configured.replace(QLatin1Char(','), QLatin1Char(';'));
    configured.replace(QLatin1Char('\r'), QLatin1Char(';'));
    configured.replace(QLatin1Char('\n'), QLatin1Char(';'));
    const auto existing = configured.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const auto &name : names) {
        auto *item = new QListWidgetItem(windows.value(name), list);
        item->setData(Qt::UserRole, name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool checked = std::any_of(existing.cbegin(), existing.cend(), [&](const QString &entry) {
            QString normalized = entry.trimmed();
            if (normalized.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) normalized.chop(4);
            QString processName = name;
            if (processName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) processName.chop(4);
            return normalized.compare(processName, Qt::CaseInsensitive) == 0;
        });
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    connect(search, &QLineEdit::textChanged, list, [list](const QString &text) {
        for (int i = 0; i < list->count(); ++i) list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
    buttons->button(QDialogButtonBox::Ok)->setText(AppLocalization::get(QStringLiteral("Ok")));
    buttons->button(QDialogButtonBox::Cancel)->setText(AppLocalization::get(QStringLiteral("Cancel")));
    connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    layout->addWidget(buttons);
    if (picker.exec() != QDialog::Accepted) return;
    QStringList values;
    QSet<QString> known;
    // 未运行的手动排除项不因进程选择对话框而丢失。
    for (const auto &entry : existing) {
        QString normalized = entry.trimmed();
        if (normalized.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) normalized.chop(4);
        if (normalized.isEmpty() || windows.keys().contains(normalized + QStringLiteral(".exe"), Qt::CaseInsensitive)) continue;
        const QString key = normalized.toCaseFolded();
        if (!known.contains(key)) { known.insert(key); values.push_back(normalized); }
    }
    for (int i = 0; i < list->count(); ++i) {
        const auto *item = list->item(i);
        if (item->checkState() != Qt::Checked) continue;
        QString name = item->data(Qt::UserRole).toString();
        if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) name.chop(4);
        const QString key = name.toCaseFolded();
        if (!known.contains(key)) { known.insert(key); values.push_back(name); }
    }
    excludedApplications_->setText(values.join(QStringLiteral("; ")));
    persist();
}

void SettingsDialog::restoreDefaults() {
    const auto answer = QMessageBox::question(this, AppLocalization::get(QStringLiteral("RestoreDefaultsTitle")),
                                              AppLocalization::get(QStringLiteral("RestoreDefaultsMessage")));
    if (answer != QMessageBox::Yes) return;
    settings_ = AppSettings{};
    loadControls(settings_);
    persist(false);
}

void SettingsDialog::done(int result) {
    // 备份回调负责恢复监听，任务完成前不能销毁设置窗口。
    if (!storageBusy_) QDialog::done(result);
}

void SettingsDialog::exportBackup() {
    if (storageBusy_) return;
    const QString filter = AppLocalization::get(QStringLiteral("EncryptedBackupFileType"))
        + QStringLiteral(" (*.pobak)");
    QFileDialog picker(this, AppLocalization::get(QStringLiteral("ExportBackupButtonContent")));
    picker.setAcceptMode(QFileDialog::AcceptSave);
    picker.setNameFilter(filter);
    picker.setDefaultSuffix(QStringLiteral("pobak"));
    if (picker.exec() != QDialog::Accepted || picker.selectedFiles().isEmpty()) return;
    const QString destination = picker.selectedFiles().first();
    storageBusy_ = true;
    setEnabled(false);
    emit storageOperationStarted();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString error = watcher->result();
        watcher->deleteLater();
        storageBusy_ = false;
        setEnabled(true);
        emit storageOperationFinished(false);
        if (error.isEmpty())
            QMessageBox::information(this, AppLocalization::get(QStringLiteral("BackupExportedTitle")),
                                     AppLocalization::get(QStringLiteral("BackupExportedMessage")));
        else
            QMessageBox::warning(this, AppLocalization::get(QStringLiteral("BackupExportFailed")), error);
    });
    const QString database = databasePath_;
    const QString settings = settingsPath_;
    watcher->setFuture(QtConcurrent::run([database, settings, destination] {
        return BackupService::exportToFile(database, settings, destination);
    }));
}

void SettingsDialog::restoreBackup() {
    if (storageBusy_) return;
    const QString filter = AppLocalization::get(QStringLiteral("EncryptedBackupFileType"))
        + QStringLiteral(" (*.pobak)");
    const QString source = QFileDialog::getOpenFileName(this,
        AppLocalization::get(QStringLiteral("RestoreBackupTitle")), QString{}, filter);
    if (source.isEmpty()) return;
    if (QMessageBox::question(this, AppLocalization::get(QStringLiteral("RestoreBackupTitle")),
            AppLocalization::get(QStringLiteral("RestoreBackupMessage")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    storageBusy_ = true;
    setEnabled(false);
    emit storageOperationStarted();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString error = watcher->result();
        watcher->deleteLater();
        storageBusy_ = false;
        setEnabled(true);
        if (error.isEmpty()) {
            settings_ = AppSettings::load(settingsPath_);
            loadControls(settings_);
            persist();
        }
        emit storageOperationFinished(error.isEmpty());
        if (!error.isEmpty()) QMessageBox::warning(this, AppLocalization::get(QStringLiteral("RestoreBackupTitle")), error);
    });
    const QString database = databasePath_;
    const QString settings = settingsPath_;
    watcher->setFuture(QtConcurrent::run([source, database, settings] {
        return BackupService::restoreFromFile(source, database, settings);
    }));
}

#include "settings_dialog.moc"
