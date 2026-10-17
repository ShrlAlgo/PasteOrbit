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
#include <QFileDialog>
#include <QFutureWatcher>
#include <QFrame>
#include <QFontMetrics>
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
#include <QShortcut>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
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

QString processKey(QString name) {
    name = name.trimmed();
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) name.chop(4);
    return name.toCaseFolded();
}

QStringList processEntries(QString text) {
    text.replace(QLatin1Char(','), QLatin1Char(';'));
    text.replace(QLatin1Char('\r'), QLatin1Char(';'));
    text.replace(QLatin1Char('\n'), QLatin1Char(';'));
    return text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
}

QIcon processRemoveIcon(const QWidget *widget, const QColor &color) {
    // 标准关闭图标在深色主题下仍可能是黑色，按主题前景色着色并保留高 DPI 尺寸。
    QPixmap pixmap = widget->style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, widget)
        .pixmap(QSize(12, 12), widget->devicePixelRatioF());
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    return QIcon(pixmap);
}

class ProcessTagArea final : public QWidget {
public:
    explicit ProcessTagArea(QWidget *parent) : QWidget(parent) {
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(width, false); }
    QSize sizeHint() const override { return {240, heightForWidth(240)}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
    void refresh() { updateGeometry(); arrange(width(), true); }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        arrange(event->size().width(), true);
    }
    void changeEvent(QEvent *event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) refresh();
    }

private:
    // 标签按实际文字宽度换行；过长名称省略显示，完整值仍保留在提示与配置中。
    int arrange(int available, bool apply) const {
        const int width = qMax(1, available);
        int x = 0, y = 0, height = 0;
        for (auto *tag : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (tag->isHidden()) continue;
            const QString text = tag->property("ProcessTagText").toString();
            const int tagWidth = qMin(width, fontMetrics().horizontalAdvance(text) + 36);
            if (x && x + tagWidth > width) { x = 0; y += 34; }
            if (apply) {
                tag->setGeometry(x, y, tagWidth, 28);
                if (auto *label = tag->findChild<QLabel *>())
                    label->setText(label->fontMetrics().elidedText(text, Qt::ElideRight, qMax(0, tagWidth - 36)));
            }
            x += tagWidth + 6;
            height = y + 28;
        }
        return height;
    }
};

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
        ensurePolished();
        view()->ensurePolished();
        if (!screen()) { QComboBox::showPopup(); return; }
        const QRect available = screen()->availableGeometry();
        const QPoint below = mapToGlobal(QPoint(0, height() + 4));
        const int spaceBelow = available.bottom() - below.y() + 1;
        const int spaceAbove = mapToGlobal(QPoint(0, -4)).y() - available.top();
        const auto margins = view()->parentWidget()->layout()->contentsMargins();
        // 根据实际行高预留完整列表空间，避免 Qt 菜单留白挤掉末项；屏幕不足时仍允许滚动。
        int contentHeight = 0;
        view()->doItemsLayout();
        for (int row = 0; row < count(); ++row) contentHeight += qMax(0, view()->sizeHintForRow(row));
        setMaxVisibleItems(count());
        view()->setMinimumHeight(qMin(contentHeight, qMax(1,
            qMax(spaceBelow, spaceAbove) - margins.top() - margins.bottom() - 2)));
        view()->parentWidget()->setMaximumHeight(available.height());
        QComboBox::showPopup();
        auto *popup = view()->parentWidget();
        if (!popup->isVisible()) return;
        // Qt 默认把当前项对齐到控件上；设置页始终优先从控件下缘展开。
        const int preferredY = below.y() + popup->height() <= available.bottom() + 1
            ? below.y() : mapToGlobal(QPoint(0, -popup->height() - 4)).y();
        popup->move(qBound(available.left(), below.x(), qMax(available.left(), available.right() - popup->width() + 1)),
                    qBound(available.top(), preferredY, qMax(available.top(), available.bottom() - popup->height() + 1)));
    }
};

class SettingsComboDelegate final : public QStyledItemDelegate {
    // 独立元对象让 Qlementine 识别为自定义委托，避免主题初始化时替换掉行高和绘制逻辑。
    Q_OBJECT
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
        : QPushButton(parent), needsModifier_(needsModifier) {
        // 录入时也注册 Shift+F10，避免改掉默认快捷键后无法再录回去。
        contextShortcut_ = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F10), this);
        contextShortcut_->setContext(Qt::WidgetShortcut);
        contextShortcut_->setEnabled(false);
        connect(contextShortcut_, &QShortcut::activated, this, [this] {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_F10, Qt::ShiftModifier);
            keyPressEvent(&event);
        });
    }

    void beginCapture() {
        previous_ = text();
        capturing_ = true;
        contextShortcut_->setEnabled(true);
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
            contextShortcut_->setEnabled(false);
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
        contextShortcut_->setEnabled(false);
        setText(value);
        emit valueCaptured(value);
        event->accept();
    }

private:
    friend class SettingsDialog;
    bool needsModifier_ = false;
    QShortcut *contextShortcut_ = nullptr;
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
    // 保留原有设置保存与对话框返回流程，但作为宿主中的普通页面，不创建独立窗口。
    setWindowFlags(Qt::Widget);
    setWindowTitle(AppLocalization::get(QStringLiteral("SettingsWindowTitle")));
    setWindowIcon(QIcon(QStringLiteral(":/PasteOrbit.ico")));
    setFocusPolicy(Qt::StrongFocus);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 4, 8, 6);
    outer->setSpacing(4);
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("Header"));
    header->setFixedHeight(36);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    auto *back = new QToolButton(header);
    back->setObjectName(QStringLiteral("SettingsBackToHistory"));
    back->setToolTip(AppLocalization::get(QStringLiteral("SettingsBackToHistory")));
    back->setAccessibleName(back->toolTip());
    back->setToolButtonStyle(Qt::ToolButtonIconOnly);
    back->setIconSize(QSize(20, 20));
    back->setFixedSize(32, 32);
    back->setAutoRaise(true);
    connect(back, &QToolButton::clicked, this, &QDialog::reject);
    headerLayout->addWidget(back);
    auto *title = new QLabel(windowTitle(), header);
    title->setProperty("LocalizationKey", QStringLiteral("SettingsWindowTitle"));
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    auto *close = new QToolButton(header);
    close->setText(QString(QChar(0x00D7)));
    close->setToolTip(AppLocalization::get(QStringLiteral("SettingsClosePanel")));
    close->setAccessibleName(close->toolTip());
    close->setObjectName(QStringLiteral("SettingsClosePanel"));
    close->setAutoRaise(true);
    close->setFixedSize(32, 32);
    connect(close, &QToolButton::clicked, this, &SettingsDialog::hideRequested);
    headerLayout->addWidget(close);
    outer->addWidget(header);
    navigation_ = new QTabBar(this);
    navigation_->setObjectName(QStringLiteral("SettingsNavigation"));
    navigation_->setExpanding(true);
    navigation_->setDrawBase(false);
    navigation_->setUsesScrollButtons(false);
    navigation_->setElideMode(Qt::ElideRight);
    navigation_->setFixedHeight(34);
    auto navigationFont = navigation_->font();
    navigationFont.setPixelSize(12);
    navigation_->setFont(navigationFont);
    tabs_ = new QStackedWidget(this);
    outer->addWidget(navigation_);
    outer->addWidget(tabs_, 1);
    connect(navigation_, &QTabBar::currentChanged, tabs_, &QStackedWidget::setCurrentIndex);
    const auto addTab = [this](QWidget *page, const QString &key) {
        page->setProperty("TabLocalizationKey", key);
        tabs_->addWidget(page);
        const int index = navigation_->addTab(AppLocalization::get(key));
        navigation_->setTabToolTip(index, AppLocalization::get(key));
    };

    auto *general = createPage(QStringLiteral("SettingsStartupSectionText"));
    startup_ = new oclero::qlementine::Switch(general);
    administrator_ = new oclero::qlementine::Switch(general);
    addCard(general, QStringLiteral("SettingsStartupCardHeader"), {}, startup_);
    addCard(general, QStringLiteral("SettingsRunAsAdministratorCardHeader"), QStringLiteral("SettingsRunAsAdministratorCardDescription"), administrator_);
    auto *monitorHeading = new QLabel(AppLocalization::get(QStringLiteral("SettingsMonitoringSectionText")), general);
    monitorHeading->setObjectName(QStringLiteral("SectionHeading"));
    pageLayout(general)->addWidget(monitorHeading);
    monitorText_ = new oclero::qlementine::Switch(general);
    monitorImages_ = new oclero::qlementine::Switch(general);
    monitorFiles_ = new oclero::qlementine::Switch(general);
    addCard(general, QStringLiteral("SettingsMonitorTextCardHeader"), {}, monitorText_);
    addCard(general, QStringLiteral("SettingsMonitorImagesCardHeader"), {}, monitorImages_);
    addCard(general, QStringLiteral("SettingsMonitorFilesCardHeader"), {}, monitorFiles_);
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
    addCard(general, QStringLiteral("SettingsThemeCardHeader"), {}, theme_);
    addCard(general, QStringLiteral("SettingsLanguageCardHeader"), {}, language_);
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
        {"SettingsPasteAsFileShortcutCardHeader", "", "PasteAsFileShortcut", false},
        {"SettingsMoreShortcutCardHeader", "SettingsMoreShortcutCardDescription", "MoreShortcut", false}};
    for (const auto &definition : shortcutDefs) {
        auto *button = new ShortcutButton(definition.global, hotkeys);
        button->setObjectName(QString::fromLatin1(definition.key));
        shortcuts_.insert(QString::fromLatin1(definition.key), button);
        const QString description = QString::fromLatin1(definition.description);
        if (!description.isEmpty()) {
            button->setToolTip(AppLocalization::get(description));
            button->setProperty("TooltipLocalizationKey", description);
        }
        addCard(hotkeys, QString::fromLatin1(definition.name), {}, button);
        connect(button, &QPushButton::clicked, button, [button] {
            button->beginCapture();
        });
        connect(button, &ShortcutButton::valueCaptured, this, [this, key = QString::fromLatin1(definition.key)](const QString &value) {
            if (key == QStringLiteral("GlobalHotKey")) settings_.globalHotKey = value;
            else if (key == QStringLiteral("PasteShortcut")) settings_.pasteShortcut = value;
            else if (key == QStringLiteral("PlainTextPasteShortcut")) settings_.plainTextPasteShortcut = value;
            else if (key == QStringLiteral("PreviewShortcut")) settings_.previewShortcut = value;
            else if (key == QStringLiteral("PinShortcut")) settings_.pinShortcut = value;
            else if (key == QStringLiteral("PasteAsFileShortcut")) settings_.pasteAsFileShortcut = value;
            else settings_.moreShortcut = value;
            persist();
        });
    }
    addTab(hotkeys, QStringLiteral("SettingsHotKeyNavigationContent"));

    auto *history = createPage(QStringLiteral("SettingsHistorySectionText"));
    retention_ = new SettingsComboBox(history);
    for (const int days : {7, 30, 90, 365}) retention_->addItem(QString::number(days), days);
    maximumEntries_ = new SettingsComboBox(history);
    for (const int count : {1000, 5000, 10000}) maximumEntries_->addItem(QString::number(count), count);
    addCard(history, QStringLiteral("SettingsRetentionCardHeader"), {}, retention_);
    addCard(history, QStringLiteral("SettingsMaxEntriesCardHeader"), {}, maximumEntries_);
    auto *cleanupHint = new QLabel(AppLocalization::get(QStringLiteral("SettingsPinnedRetentionHintText")), history);
    cleanupHint->setObjectName(QStringLiteral("MutedText"));
    cleanupHint->setWordWrap(true);
    cleanupHint->setProperty("LocalizationKey", QStringLiteral("SettingsPinnedRetentionHintText"));
    pageLayout(history)->addWidget(cleanupHint);
    auto *defaults = new QPushButton(AppLocalization::get(QStringLiteral("RestoreDefaultsButtonContent")), history);
    defaults->setProperty("LocalizationKey", QStringLiteral("RestoreDefaultsButtonContent"));
    connect(defaults, &QPushButton::clicked, this, &SettingsDialog::restoreDefaults);
    pageLayout(history)->addWidget(defaults, 0, Qt::AlignRight);
    addTab(history, QStringLiteral("SettingsHistoryNavigationContent"));

    auto *privacy = createPage(QStringLiteral("SettingsPrivacySectionText"));
    auto *excludedRow = new QWidget(privacy);
    auto *excludedLayout = new QVBoxLayout(excludedRow);
    excludedLayout->setContentsMargins(0, 0, 0, 0);
    processTags_ = new ProcessTagArea(excludedRow);
    processTags_->setObjectName(QStringLiteral("SelectedProcessTags"));
    excludedLayout->addWidget(processTags_);
    auto *inputRow = new QHBoxLayout();
    excludedApplications_ = new QLineEdit(excludedRow);
    excludedApplications_->setPlaceholderText(AppLocalization::get(QStringLiteral("ExcludedApplicationsTextBoxPlaceholder")));
    excludedApplications_->setAccessibleName(excludedApplications_->placeholderText());
    inputRow->addWidget(excludedApplications_, 1);
    auto *addProcess = new QPushButton(AppLocalization::get(QStringLiteral("AddExcludedProcess")), excludedRow);
    addProcess->setObjectName(QStringLiteral("AddExcludedProcess"));
    addProcess->setProperty("LocalizationKey", QStringLiteral("AddExcludedProcess"));
    addProcess->setAutoDefault(false);
    addProcess->setEnabled(false);
    inputRow->addWidget(addProcess);
    excludedLayout->addLayout(inputRow);
    connect(addProcess, &QPushButton::clicked, this, &SettingsDialog::addExcludedProcesses);
    connect(excludedApplications_, &QLineEdit::returnPressed, this, &SettingsDialog::addExcludedProcesses);
    connect(excludedApplications_, &QLineEdit::textChanged, addProcess, [addProcess](const QString &text) {
        addProcess->setEnabled(!text.trimmed().isEmpty());
    });
    auto *processHeader = new QHBoxLayout();
    // 带文字的折叠操作使用普通按钮，由主题统一处理文字与图标的内部居中。
    auto *processButton = new QPushButton(excludedRow);
    processButton->setObjectName(QStringLiteral("ProcessPickerToggle"));
    processButton->setText(AppLocalization::get(QStringLiteral("SelectProcessButtonContent")));
    processButton->setAutoDefault(false);
    processButton->setIcon(style()->standardIcon(QStyle::SP_ArrowRight, nullptr, processButton));
    processButton->setCheckable(true);
    auto *processSearch = new QLineEdit(excludedRow);
    processSearch->setObjectName(QStringLiteral("ProcessPickerSearch"));
    processSearch->setPlaceholderText(AppLocalization::get(QStringLiteral("ProcessSearchPlaceholder")));
    processSearch->setAccessibleName(processSearch->placeholderText());
    processSearch->hide();
    processHeader->addWidget(processSearch, 1);
    processHeader->addWidget(processButton, 0, Qt::AlignRight | Qt::AlignVCenter);
    excludedLayout->addLayout(processHeader);
    addCard(privacy, QStringLiteral("SettingsExcludedAppsCardHeader"), {}, excludedRow);
    connect(processButton, &QPushButton::toggled, this, [this, processButton, processSearch](bool expanded) {
        processButton->setIcon(style()->standardIcon(expanded ? QStyle::SP_ArrowDown : QStyle::SP_ArrowRight, nullptr, processButton));
        processSearch->setVisible(expanded);
        if (expanded) chooseProcesses();
        else {
            processList_ = nullptr;
            delete processPicker_;
            processPicker_ = nullptr;
            processSearch->clear();
        }
    });
    auto *backupRow = new QWidget(privacy);
    auto *backupLayout = new QVBoxLayout(backupRow);
    backupLayout->setContentsMargins(0, 0, 0, 0);
    auto *exportButton = new QPushButton(AppLocalization::get(QStringLiteral("ExportBackupButtonContent")), backupRow);
    auto *restoreButton = new QPushButton(AppLocalization::get(QStringLiteral("RestoreBackupButtonContent")), backupRow);
    exportButton->setProperty("LocalizationKey", QStringLiteral("ExportBackupButtonContent"));
    restoreButton->setProperty("LocalizationKey", QStringLiteral("RestoreBackupButtonContent"));
    backupLayout->addWidget(exportButton);
    backupLayout->addWidget(restoreButton);
    addCard(privacy, QStringLiteral("SettingsBackupCardHeader"), QStringLiteral("SettingsBackupCardDescription"), backupRow);
    connect(exportButton, &QPushButton::clicked, this, &SettingsDialog::exportBackup);
    connect(restoreButton, &QPushButton::clicked, this, &SettingsDialog::restoreBackup);
    addTab(privacy, QStringLiteral("SettingsPrivacyNavigationContent"));

    auto *about = createPage(QStringLiteral("SettingsAboutSectionText"));
    auto *aboutIcon = new QLabel(about);
    aboutIcon->setAlignment(Qt::AlignCenter);
    aboutIcon->setPixmap(QIcon(QStringLiteral(":/PasteOrbit.png")).pixmap(48, 48));
    pageLayout(about)->addWidget(aboutIcon);
    auto *brand = new QLabel(QStringLiteral("PasteOrbit\n%1").arg(QCoreApplication::applicationVersion()), about);
    brand->setAlignment(Qt::AlignCenter);
    brand->setObjectName(QStringLiteral("AboutBrand"));
    pageLayout(about)->addWidget(brand);
    auto *projectButton = new QPushButton(AppLocalization::get(QStringLiteral("GitHubProjectButtonText")), about);
    auto *updateButton = new QPushButton(AppLocalization::get(QStringLiteral("CheckForUpdatesButtonContent")), about);
    projectButton->setProperty("LocalizationKey", QStringLiteral("GitHubProjectButtonText"));
    updateButton->setProperty("LocalizationKey", QStringLiteral("CheckForUpdatesButtonContent"));
    addCard(about, QStringLiteral("GitHubProjectCardHeader"), {}, projectButton);
    addCard(about, QStringLiteral("SettingsUpdateCardHeader"), {}, updateButton);
    connect(projectButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/ShrlAlgo/PasteOrbit")));
    });
    connect(updateButton, &QPushButton::clicked, this, &SettingsDialog::checkUpdatesRequested);
    addTab(about, QStringLiteral("SettingsAboutNavigationContent"));

    if (auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(QApplication::style()))
        // 全局调色板在 themeChanged 后更新，下一轮事件再刷新，避免子控件落后一套主题颜色。
        connect(style, &oclero::qlementine::QlementineStyle::themeChanged, this, &SettingsDialog::applyTheme, Qt::QueuedConnection);
    navigation_->setCurrentIndex(0);
    loadControls(settings_);

    for (auto *check : {startup_, administrator_, monitorText_, monitorImages_, monitorFiles_, interceptWinV_}) {
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
}

void SettingsDialog::paintEvent(QPaintEvent *) {
    // 抽屉自身绘制不透明圆角底色，滑动时不会透出下面的历史卡片。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Window));
    painter.drawRoundedRect(QRectF(rect()), 9, 9);
}

void SettingsDialog::applyTheme() {
    // 样式表中的 palette(...) 不会随主题重算，统一从 Qlementine 当前调色板重新应用。
    const auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(QApplication::style());
    const QPalette palette = style ? style->theme().palette : qApp->palette();
    setPalette(palette);
    if (auto *back = findChild<QToolButton *>(QStringLiteral("SettingsBackToHistory")))
        back->setIcon(this->style()->standardIcon(QStyle::SP_ArrowBack, nullptr, back));
    if (auto *toggle = findChild<QPushButton *>(QStringLiteral("ProcessPickerToggle")))
        toggle->setIcon(this->style()->standardIcon(toggle->isChecked() ? QStyle::SP_ArrowDown : QStyle::SP_ArrowRight, nullptr, toggle));
    // 弹层是独立窗口；仅其表面颜色随主题重绘，选项颜色由调色板直接提供。
    for (auto *combo : {theme_, language_, retention_, maximumEntries_}) {
        combo->setPalette(palette);
        auto *view = combo->view();
        auto *popup = view->parentWidget();
        popup->setPalette(palette);
        popup->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;border-radius:8px;")
            .arg(palette.color(QPalette::Base).name(), palette.color(QPalette::Mid).name()));
        // 弹层样式重应用会覆盖子列表调色板，必须在其后同步选项与视口颜色。
        view->setPalette(palette);
        view->viewport()->setPalette(palette);
        view->viewport()->update();
    }
    // 不在对话框祖先上设置样式表，否则 Switch::style() 会变成 QStyleSheetStyle。
    navigation_->setStyleSheet(QStringLiteral(
        "QTabBar#SettingsNavigation::tab{color:%1;background:transparent;border:0;border-bottom:2px solid transparent;padding:6px 5px;}"
        "QTabBar#SettingsNavigation::tab:hover{background:%2;}"
        "QTabBar#SettingsNavigation::tab:selected{color:%3;border-bottom:2px solid %3;}")
        .arg(palette.color(QPalette::WindowText).name(), palette.color(QPalette::AlternateBase).name(),
             palette.color(QPalette::Highlight).name()));
    for (auto *label : findChildren<QLabel *>()) {
        const QString name = label->objectName();
        if (name == QStringLiteral("CardTitle"))
            label->setStyleSheet(QStringLiteral("font-weight:600;color:%1;").arg(palette.color(QPalette::WindowText).name()));
        else if (name == QStringLiteral("CardDescription") || name == QStringLiteral("MutedText"))
            label->setStyleSheet(QStringLiteral("font-size:12px;color:%1;").arg(palette.color(QPalette::PlaceholderText).name()));
        else if (name == QStringLiteral("SectionHeading"))
            label->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;margin-top:4px;color:%1;")
                .arg(palette.color(QPalette::WindowText).name()));
        else if (name == QStringLiteral("AboutBrand"))
            label->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;padding:10px;color:%1;")
                .arg(palette.color(QPalette::WindowText).name()));
    }
    // 进程选择结果写入 QLineEdit 后，文字色不能依赖父对话框样式继承。
    excludedApplications_->setStyleSheet(QStringLiteral("color:%1;selection-color:%2;selection-background-color:%3;")
        .arg(palette.color(QPalette::Text).name(), palette.color(QPalette::HighlightedText).name(),
             palette.color(QPalette::Highlight).name()));
    // 标签样式仅作用于标签区域，不影响同页的 Qlementine 开关与列表。
    processTags_->setStyleSheet(QStringLiteral(
        "QFrame#ProcessTag{background:%1;border:1px solid %2;border-radius:6px;}"
        "QLabel#ProcessTagName{background:transparent;color:%3;border:0;}"
        "QToolButton#RemoveProcessTag{background:transparent;color:%3;border:0;border-radius:4px;}"
        "QToolButton#RemoveProcessTag:hover{background:%4;}"
        "QToolButton#RemoveProcessTag:pressed{background:%2;}")
        .arg(palette.color(QPalette::AlternateBase).name(), palette.color(QPalette::Mid).name(),
             palette.color(QPalette::Text).name(), palette.color(QPalette::Base).name()));
    const QIcon removeIcon = processRemoveIcon(processTags_, palette.color(QPalette::Text));
    for (auto *remove : processTags_->findChildren<QToolButton *>(QStringLiteral("RemoveProcessTag")))
        remove->setIcon(removeIcon);
    navigation_->update();
}

QWidget *SettingsDialog::createPage(const QString &headingKey) {
    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("SettingsPage"));
    content->setBackgroundRole(QPalette::Window);
    content->setAutoFillBackground(true);
    auto *layout = new QVBoxLayout(content);
    // Qlementine 的滚动条叠加在视口内，右侧单独预留其宽度，避免覆盖卡片。
    layout->setContentsMargins(4, 6, 14, 8);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignTop);
    auto *heading = new QLabel(AppLocalization::get(headingKey), content);
    heading->setObjectName(QStringLiteral("SectionHeading"));
    heading->setProperty("LocalizationKey", headingKey);
    layout->addWidget(heading);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setBackgroundRole(QPalette::Window);
    scroll->viewport()->setAutoFillBackground(true);
    scroll->setWidget(content);
    return scroll;
}

void SettingsDialog::addCard(QWidget *page, const QString &titleKey, const QString &descriptionKey, QWidget *control) {
    QWidget *content = pageBody(page);
    auto *card = new SettingsCard(content);
    card->setObjectName(QStringLiteral("SettingsCard"));
    card->setMinimumHeight(44);
    // 窄抽屉内只把开关和快捷键放在右侧，其余控件放在说明下方，避免挤压文字。
    const bool inlineControl = qobject_cast<oclero::qlementine::Switch *>(control) || qobject_cast<ShortcutButton *>(control);
    auto *cardLayout = new QBoxLayout(inlineControl ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom, card);
    cardLayout->setContentsMargins(12, 8, 12, 8);
    cardLayout->setSpacing(8);
    auto *labels = new QVBoxLayout();
    labels->setSpacing(3);
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
    cardLayout->addLayout(labels, inlineControl ? 1 : 0);
    if (control->parentWidget() != card) control->setParent(card);
    control->setAccessibleName(title->text());
    control->setProperty("AccessibleLocalizationKey", titleKey);
    cardLayout->addWidget(control, 0, inlineControl ? Qt::AlignVCenter : Qt::Alignment{});
    auto *bodyLayout = qobject_cast<QVBoxLayout *>(content->layout());
    bodyLayout->addWidget(card);
}

void SettingsDialog::loadControls(const AppSettings &settings) {
    loading_ = true;
    startup_->setChecked(settings.startWithWindows);
    administrator_->setChecked(settings.runAsAdministrator);
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
    settings_.excludedApplications = settings.excludedApplications;
    excludedApplications_->clear();
    syncProcessTags();
    syncProcessSelection();
    const QHash<QString, QString> values{{"GlobalHotKey", settings.globalHotKey}, {"PasteShortcut", settings.pasteShortcut},
        {"PlainTextPasteShortcut", settings.plainTextPasteShortcut}, {"PreviewShortcut", settings.previewShortcut},
        {"PinShortcut", settings.pinShortcut}, {"PasteAsFileShortcut", settings.pasteAsFileShortcut},
        {"MoreShortcut", settings.moreShortcut}};
    for (auto it = values.cbegin(); it != values.cend(); ++it) if (shortcuts_.contains(it.key())) shortcuts_[it.key()]->setText(it.value());
    loading_ = false;
}

void SettingsDialog::saveControls() {
    settings_.startWithWindows = startup_->isChecked();
    settings_.runAsAdministrator = administrator_->isChecked();
    settings_.monitorText = monitorText_->isChecked();
    settings_.monitorImages = monitorImages_->isChecked();
    settings_.monitorFiles = monitorFiles_->isChecked();
    settings_.interceptWindowsClipboardShortcut = interceptWinV_->isChecked();
    settings_.themeMode = theme_->currentData().toString();
    settings_.language = language_->currentData().toString();
    settings_.retentionDays = retention_->currentData().toInt();
    settings_.maxHistoryEntries = maximumEntries_->currentData().toInt();
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
    if (!settings_.applyWindowsStartup()) {
        QMessageBox::warning(this, AppLocalization::get(QStringLiteral("SettingsSaveFailedTitle")),
                             AppLocalization::get(QStringLiteral("StartupRegistryOpenFailed")));
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
        const auto text = AppLocalization::get(tabs_->widget(i)->property("TabLocalizationKey").toString());
        navigation_->setTabText(i, text);
        navigation_->setTabToolTip(i, text);
    }
    for (auto *widget : findChildren<QWidget *>()) {
        const auto key = widget->property("AccessibleLocalizationKey").toString();
        if (!key.isEmpty()) widget->setAccessibleName(AppLocalization::get(key));
        const auto tooltipKey = widget->property("TooltipLocalizationKey").toString();
        if (!tooltipKey.isEmpty()) widget->setToolTip(AppLocalization::get(tooltipKey));
    }
    theme_->setItemText(0, themeLabel(QStringLiteral("System")));
    theme_->setItemText(1, themeLabel(QStringLiteral("Light")));
    theme_->setItemText(2, themeLabel(QStringLiteral("Dark")));
    language_->setItemText(0, languageLabel({}));
    excludedApplications_->setPlaceholderText(AppLocalization::get(QStringLiteral("ExcludedApplicationsTextBoxPlaceholder")));
    excludedApplications_->setAccessibleName(excludedApplications_->placeholderText());
    syncProcessTags();
    if (auto *toggle = findChild<QPushButton *>(QStringLiteral("ProcessPickerToggle")))
        toggle->setText(AppLocalization::get(QStringLiteral("SelectProcessButtonContent")));
    if (auto *search = findChild<QLineEdit *>(QStringLiteral("ProcessPickerSearch"))) {
        search->setPlaceholderText(AppLocalization::get(QStringLiteral("ProcessSearchPlaceholder")));
        search->setAccessibleName(search->placeholderText());
    }
    if (processList_) {
        processList_->setAccessibleName(AppLocalization::get(QStringLiteral("SelectExcludedAppsTitle")));
    }
    if (auto *back = findChild<QToolButton *>(QStringLiteral("SettingsBackToHistory"))) {
        back->setToolTip(AppLocalization::get(QStringLiteral("SettingsBackToHistory")));
        back->setAccessibleName(back->toolTip());
    }
    if (auto *close = findChild<QToolButton *>(QStringLiteral("SettingsClosePanel"))) {
        close->setToolTip(AppLocalization::get(QStringLiteral("SettingsClosePanel")));
        close->setAccessibleName(close->toolTip());
    }
}

void SettingsDialog::chooseProcesses() {
    if (processPicker_) return;
    // 展开时才枚举窗口；列表嵌入原卡片，收起即释放，避免常驻进程元数据。
    auto *host = excludedApplications_->parentWidget();
    processPicker_ = new QWidget(host);
    processPicker_->setObjectName(QStringLiteral("ProcessPicker"));
    auto *layout = new QVBoxLayout(processPicker_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *search = findChild<QLineEdit *>(QStringLiteral("ProcessPickerSearch"));
    auto *list = new QListWidget(processPicker_);
    processList_ = list;
    list->setObjectName(QStringLiteral("ProcessSelectionList"));
    list->setAccessibleName(AppLocalization::get(QStringLiteral("SelectExcludedAppsTitle")));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setTextElideMode(Qt::ElideRight);
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
    for (const auto &name : names) {
        auto *item = new QListWidgetItem(name, list);
        item->setToolTip(windows.value(name));
        item->setData(Qt::UserRole, name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setSizeHint(QSize(0, 28));
    }
    list->setFixedHeight(qBound(60, list->count() * 28 + 2, 156));
    syncProcessSelection();
    connect(search, &QLineEdit::textChanged, list, [list](const QString &text) {
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            item->setHidden(!item->text().contains(text, Qt::CaseInsensitive)
                && !item->toolTip().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const QString changed = processKey(item->data(Qt::UserRole).toString());
        const auto existing = processEntries(settings_.excludedApplications);
        const bool wasExcluded = std::any_of(existing.cbegin(), existing.cend(), [&](const QString &entry) {
            return processKey(entry) == changed;
        });
        if (changed.isEmpty() || wasExcluded == (item->checkState() == Qt::Checked)) return;
        QStringList values;
        QSet<QString> known;
        // 每次只修改当前项；过滤中的勾选和未运行的手动排除项都必须保留。
        for (const auto &entry : existing) {
            const QString key = processKey(entry);
            if (key.isEmpty() || key == changed || known.contains(key)) continue;
            known.insert(key);
            values.push_back(entry.trimmed());
        }
        if (item->checkState() == Qt::Checked && !changed.isEmpty()) values.push_back(changed);
        settings_.excludedApplications = values.join(QStringLiteral("; "));
        syncProcessTags();
        syncProcessSelection();
        if (!loading_) persist();
    });
    qobject_cast<QVBoxLayout *>(host->layout())->addWidget(processPicker_);
    search->setFocus(Qt::OtherFocusReason);
}

void SettingsDialog::syncProcessSelection() {
    if (!processList_) return;
    QSet<QString> selected;
    for (const auto &entry : processEntries(settings_.excludedApplications)) selected.insert(processKey(entry));
    const QSignalBlocker blocker(processList_);
    for (int i = 0; i < processList_->count(); ++i) {
        auto *item = processList_->item(i);
        item->setCheckState(selected.contains(processKey(item->data(Qt::UserRole).toString())) ? Qt::Checked : Qt::Unchecked);
    }
}

void SettingsDialog::syncProcessTags() {
    for (auto *tag : processTags_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        tag->hide();
        tag->deleteLater();
    }
    QSet<QString> known;
    // 同一批标签共享关闭图标，不为每个进程重复生成和着色位图。
    const QIcon removeIcon = processRemoveIcon(processTags_, palette().color(QPalette::Text));
    for (const auto &entry : processEntries(settings_.excludedApplications)) {
        const QString key = processKey(entry);
        if (key.isEmpty() || known.contains(key)) continue;
        known.insert(key);
        QString text = entry.trimmed();
        if (text.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) text.chop(4);
        auto *tag = new QFrame(processTags_);
        tag->setObjectName(QStringLiteral("ProcessTag"));
        tag->setProperty("ProcessTagText", text);
        tag->setToolTip(entry.trimmed());
        auto *row = new QHBoxLayout(tag);
        row->setContentsMargins(8, 4, 4, 4);
        row->setSpacing(4);
        auto *name = new QLabel(tag);
        name->setObjectName(QStringLiteral("ProcessTagName"));
        name->setTextFormat(Qt::PlainText);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(name, 1);
        auto *remove = new QToolButton(tag);
        remove->setObjectName(QStringLiteral("RemoveProcessTag"));
        remove->setIcon(removeIcon);
        remove->setIconSize(QSize(12, 12));
        remove->setToolButtonStyle(Qt::ToolButtonIconOnly);
        remove->setFixedSize(20, 20);
        remove->setAutoRaise(true);
        remove->setProperty("ExcludedProcessKey", key);
        remove->setToolTip(AppLocalization::format(QStringLiteral("RemoveExcludedProcess"), {text}));
        remove->setAccessibleName(remove->toolTip());
        row->addWidget(remove);
        connect(remove, &QToolButton::clicked, this, [this, key] {
            QStringList values;
            for (const auto &value : processEntries(settings_.excludedApplications))
                if (processKey(value) != key) values.push_back(value.trimmed());
            settings_.excludedApplications = values.join(QStringLiteral("; "));
            syncProcessTags();
            syncProcessSelection();
            excludedApplications_->setFocus(Qt::OtherFocusReason);
            if (!loading_) persist();
        });
        tag->show();
    }
    processTags_->setVisible(!known.isEmpty());
    static_cast<ProcessTagArea *>(processTags_)->refresh();
}

void SettingsDialog::addExcludedProcesses() {
    QStringList values = processEntries(settings_.excludedApplications);
    QSet<QString> known;
    for (const auto &value : values) known.insert(processKey(value));
    bool changed = false;
    // 手动输入仅在添加或回车时提交，未提交的输入不覆盖已有标签与排除配置。
    for (auto value : processEntries(excludedApplications_->text())) {
        const QString key = processKey(value);
        if (key.isEmpty() || known.contains(key)) continue;
        known.insert(key);
        value = value.trimmed();
        if (value.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) value.chop(4);
        values.push_back(value);
        changed = true;
    }
    excludedApplications_->clear();
    if (!changed) return;
    settings_.excludedApplications = values.join(QStringLiteral("; "));
    syncProcessTags();
    syncProcessSelection();
    if (!loading_) persist();
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
