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
#include <QMouseEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QShortcut>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QWheelEvent>
#include <QTabBar>
#include <QToolButton>
#include <QStyle>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPainterPath>
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

class ProcessList final : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    // 鼠标统一按整行处理，避免原生勾选框和行点击分别切换一次；保留键盘与无障碍勾选。
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) { QListWidget::mousePressEvent(event); return; }
        pressed_ = indexAt(event->position().toPoint());
        if (pressed_.isValid()) {
            setCurrentIndex(pressed_);
            setFocus(Qt::MouseFocusReason);
        }
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) { QListWidget::mouseReleaseEvent(event); return; }
        if (pressed_.isValid() && pressed_ == indexAt(event->position().toPoint())) {
            auto *item = itemFromIndex(pressed_);
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        }
        pressed_ = QPersistentModelIndex();
        event->accept();
    }
    void wheelEvent(QWheelEvent *event) override {
        QListWidget::wheelEvent(event);
        // 列表滚动到边界仍消耗滚轮，避免同一次操作继续滚动外层设置页。
        event->accept();
    }
private:
    QPersistentModelIndex pressed_;
};

QIcon processRemoveIcon(const QWidget *widget, const QColor &color) {
    // 标准关闭图标在深色主题下仍可能是黑色，按主题前景色着色并保留高 DPI 尺寸。
    QPixmap pixmap = widget->style()->standardIcon(QStyle::SP_TitleBarCloseButton, nullptr, widget)
        .pixmap(QSize(12, 12), widget->devicePixelRatioF());
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    return QIcon(pixmap);
}

QIcon restoreDefaultsIcon(const QToolButton *button, const QColor &color) {
    const QSize size = button->iconSize();
    const qreal scale = button->devicePixelRatioF();
    QPixmap pixmap(qRound(size.width() * scale), qRound(size.height() * scale));
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size.width() / 24.0, size.height() / 24.0);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    // 细线回转箭头按设备像素比绘制，不使用带实心底的系统图标。
    QPainterPath path;
    path.moveTo(3, 12);
    path.cubicTo(3, 17, 7, 21, 12, 21);
    path.cubicTo(17, 21, 21, 17, 21, 12);
    path.cubicTo(21, 7, 17, 3, 12, 3);
    path.cubicTo(8.5, 3, 5.5, 5, 3, 8);
    painter.drawPath(path);
    painter.drawPolyline(QPolygonF{QPointF(3, 3), QPointF(3, 8), QPointF(8, 8)});
    painter.end();
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
    friend class ::SettingsDialog;
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
    auto *defaults = new QToolButton(header);
    defaults->setObjectName(QStringLiteral("SettingsRestoreDefaults"));
    defaults->setToolTip(AppLocalization::get(QStringLiteral("RestoreDefaultsButtonContent")));
    defaults->setAccessibleName(defaults->toolTip());
    defaults->setProperty("TooltipLocalizationKey", QStringLiteral("RestoreDefaultsButtonContent"));
    defaults->setProperty("AccessibleLocalizationKey", QStringLiteral("RestoreDefaultsButtonContent"));
    defaults->setToolButtonStyle(Qt::ToolButtonIconOnly);
    defaults->setIconSize(QSize(18, 18));
    defaults->setFixedSize(28, 28);
    defaults->setAutoRaise(true);
    connect(defaults, &QToolButton::clicked, this, &SettingsDialog::restoreDefaults);
    headerLayout->addWidget(defaults);
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
    // 嵌入透明顶层窗口后，页面切换需让宿主一起刷新，避免显示旧页面的缓存。
    connect(tabs_, &QStackedWidget::currentChanged, this, [this] { window()->update(); });
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
    normalizeUnicodeText_ = new oclero::qlementine::Switch(general);
    addCard(general, QStringLiteral("SettingsMonitorTextCardHeader"), {}, monitorText_);
    addCard(general, QStringLiteral("SettingsMonitorImagesCardHeader"), {}, monitorImages_);
    addCard(general, QStringLiteral("SettingsMonitorFilesCardHeader"), {}, monitorFiles_);
    addCard(general, QStringLiteral("SettingsNormalizeUnicodeHeader"), QStringLiteral("SettingsNormalizeUnicodeDescription"), normalizeUnicodeText_);
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
        {"SettingsPlainTextShortcutCardHeader", "SettingsPlainTextShortcutCardDescription", "PlainTextPasteShortcut", false},
        {"SettingsPreviewShortcutCardHeader", "", "PreviewShortcut", false},
        {"SettingsPinShortcutCardHeader", "", "PinShortcut", false},
        {"SettingsPasteAsFileShortcutCardHeader", "", "PasteAsFileShortcut", false},
        {"SettingsMoreShortcutCardHeader", "SettingsMoreShortcutCardDescription", "MoreShortcut", false},
        {"SettingsOpenShortcutCardHeader", "SettingsOpenShortcutCardDescription", "OpenShortcut", false}};
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
        connect(button, &ShortcutButton::valueCaptured, this, [this, button, key = QString::fromLatin1(definition.key)](const QString &value) {
            if (settings_.shortcutConflicts(value, key)) {
                button->setText(button->previous_);
                QMessageBox::warning(this, AppLocalization::get(QStringLiteral("ShortcutConflictTitle")),
                    AppLocalization::format(QStringLiteral("ShortcutConflictMessage"), {value}));
                return;
            }
            if (key == QStringLiteral("GlobalHotKey")) settings_.globalHotKey = value;
            else if (key == QStringLiteral("PasteShortcut")) settings_.pasteShortcut = value;
            else if (key == QStringLiteral("PlainTextPasteShortcut")) settings_.plainTextPasteShortcut = value;
            else if (key == QStringLiteral("PreviewShortcut")) settings_.previewShortcut = value;
            else if (key == QStringLiteral("PinShortcut")) settings_.pinShortcut = value;
            else if (key == QStringLiteral("PasteAsFileShortcut")) settings_.pasteAsFileShortcut = value;
            else if (key == QStringLiteral("OpenShortcut")) settings_.openShortcut = value;
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
    addTab(history, QStringLiteral("SettingsHistoryNavigationContent"));

    auto *privacy = createPage(QStringLiteral("SettingsPrivacySectionText"));
    auto *excludedRow = new QWidget(privacy);
    auto *excludedLayout = new QVBoxLayout(excludedRow);
    excludedLayout->setContentsMargins(0, 0, 0, 0);
    processTags_ = new ProcessTagArea(excludedRow);
    processTags_->setObjectName(QStringLiteral("SelectedProcessTags"));
    excludedLayout->addWidget(processTags_);
    excludedApplications_ = new QLineEdit(excludedRow);
    excludedApplications_->setObjectName(QStringLiteral("ExcludedProcessInput"));
    excludedApplications_->setPlaceholderText(AppLocalization::get(QStringLiteral("ExcludedApplicationsTextBoxPlaceholder")));
    excludedApplications_->setAccessibleName(excludedApplications_->placeholderText());
    auto *addProcess = new QPushButton(AppLocalization::get(QStringLiteral("AddExcludedProcess")), excludedRow);
    addProcess->setObjectName(QStringLiteral("AddExcludedProcess"));
    addProcess->setProperty("LocalizationKey", QStringLiteral("AddExcludedProcess"));
    addProcess->setAutoDefault(false);
    addProcess->setEnabled(false);
    connect(addProcess, &QPushButton::clicked, this, &SettingsDialog::addExcludedProcesses);
    connect(excludedApplications_, &QLineEdit::returnPressed, this, &SettingsDialog::addExcludedProcesses);
    connect(excludedApplications_, &QLineEdit::textChanged, addProcess, [addProcess](const QString &text) {
        addProcess->setEnabled(!text.trimmed().isEmpty());
    });
    connect(excludedApplications_, &QLineEdit::textChanged, this, &SettingsDialog::filterProcesses);
    auto *processButton = new QPushButton(AppLocalization::get(QStringLiteral("SelectProcessButtonContent")), excludedRow);
    processButton->setObjectName(QStringLiteral("ProcessPickerToggle"));
    processButton->setProperty("LocalizationKey", QStringLiteral("SelectProcessButtonContent"));
    processButton->setAutoDefault(false);
    processButton->setCheckable(true);
    auto *actionsRow = new QHBoxLayout();
    actionsRow->addWidget(excludedApplications_, 1);
    actionsRow->addWidget(addProcess);
    actionsRow->addWidget(processButton);
    excludedLayout->addLayout(actionsRow);
    connect(processButton, &QPushButton::toggled, this, [this](bool expanded) {
        if (expanded) chooseProcesses();
        if (processPicker_) processPicker_->setVisible(expanded);
    });
    connect(excludedApplications_, &QLineEdit::textEdited, processButton, [processButton](const QString &) {
        processButton->setChecked(true);
    });
    addCard(privacy, QStringLiteral("SettingsExcludedAppsCardHeader"),
            QStringLiteral("SettingsExcludedAppsCardDescription"), excludedRow);
    auto *backupRow = new QWidget(privacy);
    auto *backupLayout = new QHBoxLayout(backupRow);
    backupLayout->setContentsMargins(0, 0, 0, 0);
    auto *exportButton = new QPushButton(AppLocalization::get(QStringLiteral("ExportBackupButtonContent")), backupRow);
    auto *restoreButton = new QPushButton(AppLocalization::get(QStringLiteral("RestoreBackupButtonContent")), backupRow);
    exportButton->setProperty("LocalizationKey", QStringLiteral("ExportBackupButtonContent"));
    restoreButton->setProperty("LocalizationKey", QStringLiteral("RestoreBackupButtonContent"));
    backupLayout->addWidget(exportButton, 1);
    backupLayout->addWidget(restoreButton, 1);
    addCard(privacy, QStringLiteral("SettingsBackupCardHeader"), QStringLiteral("SettingsBackupCardDescription"), backupRow);
    connect(exportButton, &QPushButton::clicked, this, &SettingsDialog::exportBackup);
    connect(restoreButton, &QPushButton::clicked, this, &SettingsDialog::restoreBackup);
    addTab(privacy, QStringLiteral("SettingsPrivacyNavigationContent"));

    auto *about = createPage(QStringLiteral("SettingsAboutSectionText"));
    auto *aboutIcon = new QLabel(about);
    aboutIcon->setAlignment(Qt::AlignCenter);
    aboutIcon->setPixmap(QIcon(QStringLiteral(":/PasteOrbit.png")).pixmap(48, 48));
    pageLayout(about)->addWidget(aboutIcon);
    auto *brand = new QLabel(QStringLiteral("%1\n%2").arg(AppLocalization::get(QStringLiteral("ApplicationDisplayName")), QCoreApplication::applicationVersion()), about);
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

    for (auto *check : {startup_, administrator_, monitorText_, monitorImages_, monitorFiles_, normalizeUnicodeText_, interceptWinV_}) {
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
    if (auto *defaults = findChild<QToolButton *>(QStringLiteral("SettingsRestoreDefaults"))) {
        defaults->setIcon(restoreDefaultsIcon(defaults, palette.color(QPalette::PlaceholderText)));
    }
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
    // 滚动的局部搬移可能未刷新透明宿主；update 会合并同一帧内的重绘请求。
    connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { window()->update(); });
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
    normalizeUnicodeText_->setChecked(settings.normalizeUnicodeText);
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
        {"MoreShortcut", settings.moreShortcut}, {"OpenShortcut", settings.openShortcut}};
    for (auto it = values.cbegin(); it != values.cend(); ++it) if (shortcuts_.contains(it.key())) shortcuts_[it.key()]->setText(it.value().isEmpty() ? AppLocalization::get(QStringLiteral("ShortcutNotSet")) : it.value());
    loading_ = false;
}

void SettingsDialog::saveControls() {
    settings_.startWithWindows = startup_->isChecked();
    settings_.runAsAdministrator = administrator_->isChecked();
    settings_.monitorText = monitorText_->isChecked();
    settings_.normalizeUnicodeText = normalizeUnicodeText_->isChecked();
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
    if (settings_.openShortcut.isEmpty())
        shortcuts_[QStringLiteral("OpenShortcut")]->setText(AppLocalization::get(QStringLiteral("ShortcutNotSet")));
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
    if (processList_)
        processList_->setAccessibleName(AppLocalization::get(QStringLiteral("SelectExcludedAppsTitle")));
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
    auto *host = excludedApplications_->parentWidget();
    processPicker_ = new QWidget(host);
    processPicker_->setObjectName(QStringLiteral("ProcessPicker"));
    auto *layout = new QVBoxLayout(processPicker_);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *list = new ProcessList(processPicker_);
    processList_ = list;
    list->setObjectName(QStringLiteral("ProcessSelectionList"));
    list->setAccessibleName(AppLocalization::get(QStringLiteral("SelectExcludedAppsTitle")));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setUniformItemSizes(true);
    list->setTextElideMode(Qt::ElideRight);
    list->setFixedHeight(66);
    layout->addWidget(list);
    qobject_cast<QVBoxLayout *>(host->layout())->addWidget(processPicker_);
    connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const QString changed = processKey(item->data(Qt::UserRole).toString());
        const auto existing = processEntries(settings_.excludedApplications);
        const bool wasExcluded = std::any_of(existing.cbegin(), existing.cend(), [&](const QString &entry) {
            return processKey(entry) == changed;
        });
        if (changed.isEmpty() || wasExcluded == (item->checkState() == Qt::Checked)) return;
        QStringList values;
        QSet<QString> known;
        // 筛选只改变可见项，不丢失其他勾选和未运行的手动排除项。
        for (const auto &entry : existing) {
            const QString key = processKey(entry);
            if (key.isEmpty() || key == changed || known.contains(key)) continue;
            known.insert(key);
            values.push_back(entry.trimmed());
        }
        if (item->checkState() == Qt::Checked) values.push_back(changed);
        settings_.excludedApplications = values.join(QStringLiteral("; "));
        syncProcessTags();
        syncProcessSelection();
        if (!loading_) persist();
    });
    auto *watcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, QString>>::finished, this, [this, watcher, list] {
        const auto windows = watcher->result();
        watcher->deleteLater();
        const QSignalBlocker blocker(list);
        QStringList names = windows.keys();
        names.sort(Qt::CaseInsensitive);
        for (const auto &name : names) {
            auto *item = new QListWidgetItem(name, list);
            item->setToolTip(windows.value(name));
            item->setData(Qt::UserRole, name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setSizeHint(QSize(0, 32));
        }
        syncProcessSelection();
        filterProcesses(excludedApplications_->text());
    });
    // 窗口与进程查询放到后台；同一设置页会话复用候选，不随每次输入重新枚举。
    watcher->setFuture(QtConcurrent::run([] {
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
            static_cast<QHash<QString, QString> *>(reinterpret_cast<void *>(parameter))->insert(
                name, QStringLiteral("%1 (%2)").arg(QString::fromWCharArray(title), name));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));
        return windows;
    }));
}

void SettingsDialog::filterProcesses(const QString &text) {
    if (!processList_) return;
    const QString query = text.trimmed();
    int visible = 0;
    for (int row = 0; row < processList_->count(); ++row) {
        auto *item = processList_->item(row);
        const bool matches = item->text().contains(query, Qt::CaseInsensitive)
            || item->toolTip().contains(query, Qt::CaseInsensitive);
        item->setHidden(!matches);
        if (matches) ++visible;
    }
    processList_->setFixedHeight(qBound(66, visible * 32 + 2, 162));
}

void SettingsDialog::syncProcessSelection() {
    if (!processList_) return;
    QSet<QString> selected;
    for (const auto &entry : processEntries(settings_.excludedApplications)) selected.insert(processKey(entry));
    const QSignalBlocker blocker(processList_);
    for (int row = 0; row < processList_->count(); ++row) {
        auto *item = processList_->item(row);
        item->setCheckState(selected.contains(processKey(item->data(Qt::UserRole).toString())) ? Qt::Checked : Qt::Unchecked);
    }
}

void SettingsDialog::syncProcessTags() {
    QHash<QString, QWidget *> tags;
    for (auto *tag : processTags_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!tag->isHidden()) tags.insert(tag->property("ExcludedProcessKey").toString(), tag);
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
        // 保留未变化的标签，避免每次选择进程都重建整组控件和布局。
        if (auto *tag = tags.take(key)) {
            tag->setProperty("ProcessTagText", text);
            tag->setToolTip(entry.trimmed());
            auto *remove = tag->findChild<QToolButton *>();
            remove->setToolTip(AppLocalization::format(QStringLiteral("RemoveExcludedProcess"), {text}));
            remove->setAccessibleName(remove->toolTip());
            continue;
        }
        auto *tag = new QFrame(processTags_);
        tag->setObjectName(QStringLiteral("ProcessTag"));
        tag->setProperty("ExcludedProcessKey", key);
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
    for (auto *tag : tags) {
        tag->hide();
        tag->deleteLater();
    }
    processTags_->setVisible(!known.isEmpty());
    static_cast<ProcessTagArea *>(processTags_)->refresh();
}

void SettingsDialog::addExcludedProcesses() {
    QStringList values = processEntries(settings_.excludedApplications);
    QSet<QString> known;
    for (auto &value : values) {
        value = value.trimmed();
        known.insert(processKey(value));
    }
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
                                              AppLocalization::get(QStringLiteral("RestoreDefaultsMessage")),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    // 统一重置全部配置并同步各页控件，不局限于当前选项卡。
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
