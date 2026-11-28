#include "CollectionDialog.h"
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStylePainter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace tb::ui {
namespace {
class ResponsiveRow : public QWidget {
public:
    explicit ResponsiveRow(QWidget* parent) : QWidget(parent),
        row_(new QBoxLayout(QBoxLayout::LeftToRight, this)) {
        row_->setSizeConstraint(QLayout::SetNoConstraint);
        row_->setContentsMargins(0, 0, 0, 0);
        row_->setSpacing(6);
    }

    void addWidget(QWidget* widget, int stretch = 0) { row_->addWidget(widget, stretch); }
    void addStretch() { row_->addStretch(); }
    QSize sizeHint() const override { return {rowWidth(), QWidget::sizeHint().height()}; }
    QSize minimumSizeHint() const override { return {0, QWidget::minimumSizeHint().height()}; }

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest) {
            const auto direction = width() < rowWidth() ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
            if (row_->direction() != direction) row_->setDirection(direction);
        }
        return QWidget::event(event);
    }

private:
    int rowWidth() const {
        int width = 0;
        for (int i = 0; i < row_->count(); ++i) {
            const auto* item = row_->itemAt(i);
            const auto* widget = row_->itemAt(i)->widget();
            if (!widget) continue;
            const auto* group = dynamic_cast<const ResponsiveRow*>(widget);
            auto minimum = group ? group->rowWidth() : item->minimumSize().width();
            if (row_->stretch(i)) minimum = std::max(minimum, widget->fontMetrics().horizontalAdvance(QString(14, u'0')));
            width += minimum + (width ? row_->spacing() : 0);
        }
        return width;
    }

    QBoxLayout* row_;
};

class CollectionCombo : public QComboBox {
public:
    explicit CollectionCombo(QWidget* parent) : QComboBox(parent) {
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        view()->setTextElideMode(Qt::ElideRight);
        view()->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        if (isEditable()) {
            QComboBox::paintEvent(event);
            return;
        }
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const auto field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight, field.width());
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

QString toBase64(const SecureBytes& bytes) {
    const SecureBytes encoded(bytes.bytes().toBase64());
    return QString::fromLatin1(encoded.bytes());
}

const QString NoProject = QStringLiteral("Без проекта");
QString projectLabel(const QString& project) {
    if (project.isEmpty()) return NoProject;
    if (project == NoProject) return QStringLiteral("«%1» (проект)").arg(project);
    return project;
}

QList<model::VaultModel::Node> vaultPath(const model::VaultModel& vault, model::Id id) {
    QList<model::VaultModel::Node> path;
    const auto& nodes = vault.nodes();
    for (int depth = 0; !id.isNull() && depth < model::VaultModel::MaxDepth; ++depth) {
        const auto node = nodes.constFind(id);
        if (node == nodes.cend()) return {};
        path.prepend(*node);
        id = node->parent;
    }
    return id.isNull() ? path : QList<model::VaultModel::Node>();
}
}

CollectionPane::CollectionPane(Application& app, std::function<bool(QWidget*)> approveUpgrade,
                               int revealSeconds, ClipboardGuard& clipboard, QWidget* parent)
    : QWidget(parent), app_(app), approveUpgrade_(std::move(approveUpgrade)), clipboard_(&clipboard) {
    setObjectName("collectionPane");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    heading_ = new QLabel(this);
    heading_->setObjectName("collectionDetailHeading");
    heading_->setTextFormat(Qt::PlainText);
    heading_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading_->setWordWrap(true);
    heading_->hide();
    layout->addWidget(heading_);

    listPanel_ = new QWidget(this);
    listPanel_->setObjectName("collectionListPane");
    auto* sidebar = new QVBoxLayout(listPanel_);
    sidebar->setContentsMargins(0, 0, 0, 0);
    auto* listHeading = new QLabel(QStringLiteral("Коллекция агента"), listPanel_);
    listHeading->setObjectName("collectionHeading");
    listHeading->setWordWrap(true);
    sidebar->addWidget(listHeading);
    listStatus_ = new QLabel(listPanel_);
    listStatus_->setObjectName("collectionListStatus");
    listStatus_->setWordWrap(true);
    sidebar->addWidget(listStatus_);
    access_ = new QPushButton(QStringLiteral("Открыть доступ"), listPanel_);
    access_->setObjectName("collectionAccess");
    sidebar->addWidget(access_, 0, Qt::AlignLeft);
    items_ = new QTreeWidget(listPanel_);
    items_->setObjectName("collectionItems");
    items_->setAccessibleName(QStringLiteral("Записи коллекции агента"));
    items_->setHeaderLabels({QStringLiteral("Имя"), QStringLiteral("Проект")});
    items_->setRootIsDecorated(false);
    items_->setIndentation(0);
    items_->setUniformRowHeights(true);
    items_->setTextElideMode(Qt::ElideRight);
    items_->header()->setSectionResizeMode(QHeaderView::Stretch);
    items_->setMinimumHeight(0);
    sidebar->addWidget(items_, 1);
    auto* listActions = new ResponsiveRow(listPanel_);
    sidebar->addWidget(listActions);
    add_ = new QPushButton(QStringLiteral("Добавить"), listPanel_);
    add_->setObjectName("collectionAdd");
    listActions->addWidget(add_);
    reload_ = new QPushButton(QStringLiteral("Обновить"), listPanel_);
    reload_->setObjectName("collectionReload");
    listActions->addWidget(reload_);

    editor_ = new QWidget(this);
    layout->addWidget(editor_);
    auto* fields = new QVBoxLayout(editor_);
    fields->setContentsMargins(0, 0, 0, 0);
    auto* metadata = new ResponsiveRow(editor_);
    fields->addWidget(metadata);
    auto* projectField = new QWidget(metadata);
    metadata->addWidget(projectField, 1);
    auto* projectLayout = new QVBoxLayout(projectField);
    projectLayout->setContentsMargins(0, 0, 0, 0);
    auto* projectLabel = new QLabel(QStringLiteral("&Проект"), editor_);
    project_ = new CollectionCombo(editor_);
    project_->setObjectName("collectionProject");
    project_->setEditable(true);
    project_->setInsertPolicy(QComboBox::NoInsert);
    project_->setCompleter(nullptr);
    project_->lineEdit()->setPlaceholderText(NoProject);
    project_->setToolTip(QStringLiteral("Выберите проект, введите новое имя или оставьте поле пустым."));
    projectLabel->setBuddy(project_);
    projectLayout->addWidget(projectLabel);
    projectLayout->addWidget(project_);
    auto* nameField = new QWidget(metadata);
    metadata->addWidget(nameField, 1);
    auto* nameLayout = new QVBoxLayout(nameField);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    auto* nameLabel = new QLabel(QStringLiteral("&Имя"), editor_);
    name_ = new QLineEdit(editor_);
    name_->setObjectName("collectionName");
    nameLabel->setBuddy(name_);
    nameLayout->addWidget(nameLabel);
    nameLayout->addWidget(name_);
    auto* valueRow = new ResponsiveRow(editor_);
    auto* valueLabel = new QLabel(QStringLiteral("&Значение"), editor_);
    valueRow->addWidget(valueLabel);
    valueRow->addStretch();
    format_ = new QComboBox(editor_);
    format_->setObjectName("collectionFormat");
    format_->setAccessibleName(QStringLiteral("Формат значения"));
    format_->setToolTip(QStringLiteral("Для двоичных и многострочных значений используйте Base64."));
    format_->addItems({QStringLiteral("Текст (UTF-8)"), QStringLiteral("Base64")});
    valueRow->addWidget(format_);
    fields->addWidget(valueRow);
    value_ = new QLineEdit(editor_);
    value_->setObjectName("collectionValue");
    value_->setEchoMode(QLineEdit::Password);
    value_->setContextMenuPolicy(Qt::NoContextMenu);
    value_->setDragEnabled(false);
    value_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    // Qt's default limit is smaller than a Base64-encoded 32 KiB value.
    value_->setMaxLength(65536);
    valueLabel->setBuddy(value_);
    auto* valueActions = new ResponsiveRow(editor_);
    valueActions->addWidget(value_, 1);
    auto* valueButtons = new ResponsiveRow(valueActions);
    reveal_ = new QPushButton(QStringLiteral("Показать"), editor_);
    reveal_->setObjectName("collectionReveal");
    copy_ = new QPushButton(QStringLiteral("Копировать значение"), editor_);
    copy_->setObjectName("collectionCopyValue");
    valueButtons->addWidget(reveal_);
    valueButtons->addWidget(copy_);
    valueActions->addWidget(valueButtons);
    fields->addWidget(valueActions);
    revealedValue_ = new QPlainTextEdit(editor_);
    revealedValue_->setObjectName("collectionRevealedValue");
    revealedValue_->setAccessibleName(QStringLiteral("Временно показанное значение"));
    revealedValue_->setReadOnly(true);
    revealedValue_->setUndoRedoEnabled(false);
    revealedValue_->setTextInteractionFlags(Qt::NoTextInteraction);
    revealedValue_->setFocusPolicy(Qt::NoFocus);
    revealedValue_->setContextMenuPolicy(Qt::NoContextMenu);
    revealedValue_->setAcceptDrops(false);
    revealedValue_->setMaximumHeight(96);
    revealedValue_->hide();
    fields->addWidget(revealedValue_);
    revealTimer_ = new QTimer(this);
    revealTimer_->setObjectName("collectionRevealTimer");
    revealTimer_->setSingleShot(true);
    setRevealSeconds(revealSeconds);
    connect(revealTimer_, &QTimer::timeout, this, &CollectionPane::hideValue);
    auto* actions = new ResponsiveRow(editor_);
    save_ = new QPushButton(QStringLiteral("Сохранить"), editor_);
    save_->setObjectName("collectionSave");
    remove_ = new QPushButton(QStringLiteral("Удалить…"), editor_);
    remove_->setObjectName("collectionDelete");
    actions->addWidget(save_);
    actions->addWidget(remove_);
    actions->addStretch();
    fields->addWidget(actions);
    auto* transfers = new ResponsiveRow(editor_);
    copyToVault_ = new QPushButton(QStringLiteral("Копировать в хранилище…"), editor_);
    copyToVault_->setObjectName("collectionCopyToVault");
    moveToVault_ = new QPushButton(QStringLiteral("Переместить в хранилище…"), editor_);
    moveToVault_->setObjectName("collectionMoveToVault");
    transfers->addWidget(copyToVault_);
    transfers->addWidget(moveToVault_);
    transfers->addStretch();
    fields->addWidget(transfers);
    actionPanel_ = new QWidget(this);
    actionPanel_->setObjectName("collectionActionPanel");
    auto* panel = new QVBoxLayout(actionPanel_);
    panel->setContentsMargins(0, 0, 0, 0);
    actionMessage_ = new QLabel(actionPanel_);
    actionMessage_->setTextFormat(Qt::PlainText);
    actionMessage_->setWordWrap(true);
    actionMessage_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    panel->addWidget(actionMessage_);
    transferFields_ = new QWidget(actionPanel_);
    transferFields_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    transferForm_ = new QFormLayout(transferFields_);
    transferForm_->setContentsMargins(0, 0, 0, 0);
    transferForm_->setRowWrapPolicy(QFormLayout::WrapLongRows);
    transferForm_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    destination_ = new CollectionCombo(transferFields_);
    destination_->setObjectName("collectionTransferDestination");
    destination_->setPlaceholderText(QStringLiteral("Выберите проект или группу"));
    destination_->setMinimumContentsLength(10);
    transferForm_->addRow(QStringLiteral("&Назначение"), destination_);
    newProject_ = new QLineEdit(transferFields_);
    newProject_->setObjectName("collectionTransferNewProject");
    transferForm_->addRow(QStringLiteral("&Имя нового проекта"), newProject_);
    panel->addWidget(transferFields_);
    auto* actionButtons = new ResponsiveRow(actionPanel_);
    confirm_ = new QPushButton(actionPanel_);
    confirm_->setObjectName("collectionConfirm");
    cancel_ = new QPushButton(QStringLiteral("Отмена"), actionPanel_);
    cancel_->setObjectName("collectionCancel");
    actionButtons->addWidget(confirm_);
    actionButtons->addWidget(cancel_);
    actionButtons->addStretch();
    panel->addWidget(actionButtons);
    layout->addWidget(actionPanel_);
    actionPanel_->hide();
    status_ = new QLabel(this);
    status_->setObjectName("collectionStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(status_);
    layout->addStretch();
    for (auto* button : {access_, add_, reload_, save_, remove_, reveal_, copy_, copyToVault_, moveToVault_, confirm_, cancel_})
        button->setAutoDefault(false);

    connect(access_, &QPushButton::clicked, this, &CollectionPane::accessRequested);
    connect(items_, &QTreeWidget::currentItemChanged, this, [this](const QTreeWidgetItem* item) {
        if (!item) return;
        const Item requested{item->data(0, Qt::UserRole).toString(), item->text(0)};
        selectInTree(selected_);
        hideValue();
        selectItem(requested);
    });
    connect(add_, &QPushButton::clicked, this, [this] {
        discardThen([this] {
            const auto project = scope_.value_or(selected_ ? selected_->project : QString());
            selected_.reset();
            editItem({project, {}}, {});
            selectInTree(std::nullopt);
            showStatus({});
            emit selectionActivated();
            name_->setFocus();
        });
    });
    connect(reload_, &QPushButton::clicked, this, [this] { discardThen([this] { reload(); }); });
    connect(save_, &QPushButton::clicked, this, &CollectionPane::save);
    connect(remove_, &QPushButton::clicked, this, &CollectionPane::remove);
    connect(copyToVault_, &QPushButton::clicked, this, [this] { transfer(false); });
    connect(moveToVault_, &QPushButton::clicked, this, [this] { transfer(true); });
    connect(reveal_, &QPushButton::clicked, this, &CollectionPane::toggleReveal);
    connect(copy_, &QPushButton::clicked, this, &CollectionPane::copyCurrentValue);
    connect(confirm_, &QPushButton::clicked, this, &CollectionPane::confirmAction);
    connect(cancel_, &QPushButton::clicked, this, &CollectionPane::cancelAction);
    connect(destination_, &QComboBox::currentIndexChanged, this, &CollectionPane::refreshAction);
    connect(newProject_, &QLineEdit::textChanged, this, &CollectionPane::refreshAction);
    const auto fieldChanged = [this] { invalidateCopy(); hideValue(); refreshButtons(); };
    connect(name_, &QLineEdit::textChanged, this, fieldChanged);
    connect(project_, &QComboBox::editTextChanged, this, fieldChanged);
    connect(value_, &QLineEdit::textChanged, this, fieldChanged);
    connect(project_, &QComboBox::activated, this, [this](int index) {
        project_->setEditText(project_->itemData(index).toString());
    });
    connect(value_, &QLineEdit::textEdited, this, [this] {
        valueEdited_ = true;
        refreshButtons();
    });
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int index) { changeFormat(index == 1); });
    connect(&app_, &Application::stateChanged, this, [this] {
        if (generation_ && !currentSession()) clearSession();
    });
    connect(&app_, &Application::busyChanged, this, [this](bool busy) {
        if (busy) hideValue();
        refreshButtons();
    });
    window()->installEventFilter(this);
    deactivate();
    refillList();
}

bool CollectionPane::currentSession() const {
    return generation_ && app_.gate() == Application::Gate::Ready && app_.generation() == *generation_;
}

bool CollectionPane::available() {
    if (!currentSession()) return false;
    if (!app_.busy()) return true;
    showStatus(QStringLiteral("Выполняется другая операция. Повторите попытку."));
    return false;
}

bool CollectionPane::dirty() const {
    return editing_ && (valueEdited_ || name_->text() != saved_.name || project_->currentText() != saved_.project);
}

bool CollectionPane::sourceStill(const Item& item, const SecureBytes& value) const {
    return isVisible() && currentSession() && item == saved_ && value.bytes() == savedValue_.bytes();
}

void CollectionPane::beginAction(const QString& message, const QString& button, Action action, bool transfer) {
    if (pending_ || !available()) return;
    invalidateCopy();
    hideValue();
    pendingSource_ = saved_;
    pendingValue_ = SecureBytes(QByteArray(savedValue_.bytes().constData(), savedValue_.size()));
    pending_ = std::move(action);
    showStatus({});
    actionMessage_->setText(message);
    confirm_->setText(button);
    transferFields_->setVisible(transfer);
    actionPanel_->show();
    refreshButtons();
    if (transfer) destination_->setFocus();
    else cancel_->setFocus();
}

void CollectionPane::discardThen(std::function<void()> action) {
    if (!available() || pending_) return;
    if (!dirty()) {
        action();
        return;
    }
    beginAction(QStringLiteral("Сбросить несохранённые изменения?"), QStringLiteral("Сбросить"),
                [action = std::move(action)](const Item&, const SecureBytes&) { action(); });
}

void CollectionPane::cancelAction() {
    pending_ = {};
    pendingSource_ = {};
    pendingValue_.clear();
    actionMessage_->clear();
    actionPanel_->hide();
    refreshButtons();
}

void CollectionPane::confirmAction() {
    if (!pending_ || !confirm_->isEnabled() || !available()) return;
    const auto source = std::move(pendingSource_);
    const auto original = std::move(pendingValue_);
    const auto action = std::move(pending_);
    cancelAction();
    if (!sourceStill(source, original)) {
        showStatus(QStringLiteral("Выбранная запись изменилась. Повторите действие."));
        return;
    }
    action(source, original);
}

void CollectionPane::refreshAction() {
    if (!pending_) return;
    const bool transfer = !transferFields_->isHidden();
    const bool creating = transfer && destination_->currentIndex() >= 0 && !destination_->currentData().isValid();
    transferForm_->setRowVisible(newProject_, creating);
    confirm_->setEnabled(currentSession() && !app_.busy() &&
        (!transfer || (destination_->currentIndex() >= 0 && (!creating || !newProject_->text().trimmed().isEmpty()))));
}

void CollectionPane::setContext(std::optional<QString> project, model::Id destination, QString filter, bool showEntries) {
    vaultDestination_ = destination;
    filter = filter.trimmed();
    if (scope_ == project && filter_ == filter && contextAvailable_ == showEntries) return;
    const bool scopeChanged = scope_ != project || contextAvailable_ != showEntries;
    scope_ = std::move(project);
    filter_ = std::move(filter);
    contextAvailable_ = showEntries;
    if (scopeChanged || (selected_ && !matches(*selected_))) deactivate();
    refillList();
}

void CollectionPane::authorize(quint64 generation) {
    if (app_.gate() != Application::Gate::Ready || app_.busy() || generation != app_.generation()) return;
    if (!currentSession()) {
        clearSession();
        generation_ = generation;
    }
    refresh();
    focusList();
}

void CollectionPane::clearSession() {
    generation_.reset();
    deactivate();
    replaceProjects({});
}

void CollectionPane::refresh() {
    if (!currentSession() || app_.busy()) return;
    QMap<QString,QStringList> projects;
    const auto error = app_.listCollection(projects);
    if (!error.isEmpty()) {
        deactivate();
        replaceProjects({});
        listStatus_->setText(error);
        listStatus_->show();
        showStatus(error);
        return;
    }
    if (!dirty() && !pending_ && selected_ && !projects.value(selected_->project).contains(selected_->name))
        deactivate();
    replaceProjects(std::move(projects));
}

void CollectionPane::focusList() {
    (currentSession() ? static_cast<QWidget*>(items_) : access_)->setFocus();
}

void CollectionPane::setRevealSeconds(int seconds) {
    revealTimer_->setInterval(seconds * 1000);
}

void CollectionPane::deactivate() {
    cancelAction();
    destination_->clear();
    newProject_->clear();
    editItem({}, {}, false);
    selected_.reset();
    selectInTree(std::nullopt);
    showStatus({});
}

void CollectionPane::confirmNavigation(std::function<void()> action) {
    if (pending_) return;
    if (!dirty()) {
        action();
        return;
    }
    discardThen([this, action = std::move(action)] {
        deactivate();
        action();
    });
}

bool CollectionPane::event(QEvent* event) {
    if (event->type() == QEvent::Hide && revealTimer_) {
        invalidateCopy();
        hideValue();
    }
    return QWidget::event(event);
}

bool CollectionPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window() && event->type() == QEvent::WindowDeactivate) {
        invalidateCopy();
        hideValue();
    }
    return QWidget::eventFilter(watched, event);
}

void CollectionPane::refreshButtons() {
    const bool enabled = currentSession() && !app_.busy() && !pending_;
    editor_->setEnabled(enabled && editing_);
    editor_->setVisible(editing_);
    heading_->setVisible(editing_);
    access_->setEnabled(app_.gate() == Application::Gate::Ready && !app_.busy());
    items_->setEnabled(enabled);
    add_->setEnabled(enabled && contextAvailable_);
    reload_->setEnabled(enabled);
    save_->setEnabled(enabled && dirty() && !name_->text().isEmpty());
    remove_->setEnabled(enabled && !saved_.name.isEmpty());
    copyToVault_->setEnabled(remove_->isEnabled());
    moveToVault_->setEnabled(remove_->isEnabled());
    reveal_->setEnabled(enabled && editing_ && !value_->text().isEmpty());
    copy_->setEnabled(reveal_->isEnabled());
    refreshAction();
}

void CollectionPane::showStatus(const QString& message) {
    status_->setText(message);
    status_->setVisible(!message.isEmpty());
}

void CollectionPane::hideValue() {
    revealTimer_->stop();
    revealedValue_->hide();
    revealedValue_->clear();
    reveal_->setText(QStringLiteral("Показать"));
}

void CollectionPane::invalidateCopy() {
    ++contentVersion_;
    if (clipboard_) clipboard_->cancelPendingCopy();
}

void CollectionPane::toggleReveal() {
    if (!available() || !isVisible() || !editing_ || value_->text().isEmpty()) return;
    if (!revealedValue_->isHidden()) { hideValue(); return; }
    revealedValue_->setPlainText(value_->text());
    revealedValue_->show();
    reveal_->setText(QStringLiteral("Скрыть"));
    revealTimer_->start();
}

void CollectionPane::copyCurrentValue() {
    if (!clipboard_ || !available() || !isVisible() || !editing_ || value_->text().isEmpty()) return;
    const auto version = contentVersion_;
    QString error;
    const bool copied = clipboard_->copySecret(value_->text(), &error);
    if (!currentSession() || !isVisible() || contentVersion_ != version) return;
    showStatus(copied ? QStringLiteral("Значение скопировано. Буфер очистится автоматически.") : error);
}

void CollectionPane::replaceProjects(QMap<QString,QStringList> projects) {
    const auto previous = projects_.keys();
    projects_ = std::move(projects);
    const auto project = project_->currentText();
    const QSignalBlocker blockProject(project_);
    project_->clear();
    project_->addItem(NoProject, QString());
    for (const auto& name : projects_.keys()) {
        if (!name.isEmpty()) project_->addItem(projectLabel(name), name);
    }
    project_->setCurrentIndex(project_->findData(project));
    project_->setEditText(project);
    refillList();
    if (previous != projects_.keys()) emit projectsChanged();
}

bool CollectionPane::matches(const Item& item) const {
    return contextAvailable_ && (!scope_ || *scope_ == item.project)
        && (filter_.isEmpty() || item.name.contains(filter_, Qt::CaseInsensitive)
            || projectLabel(item.project).contains(filter_, Qt::CaseInsensitive));
}

void CollectionPane::refillList() {
    const bool authorized = currentSession();
    const QSignalBlocker block(items_);
    items_->clear();
    items_->setHeaderHidden(scope_.has_value());
    items_->setColumnHidden(1, scope_.has_value());
    for (auto it = projects_.cbegin(); authorized && it != projects_.cend(); ++it) {
        const auto& project = it.key();
        auto entries = it.value();
        entries.sort(Qt::CaseSensitive);
        for (const auto& name : entries) {
            if (!matches({project, name})) continue;
            auto* row = new QTreeWidgetItem(items_, {name, projectLabel(project)});
            row->setData(0, Qt::UserRole, project);
            row->setToolTip(0, name);
            row->setToolTip(1, projectLabel(project));
        }
    }
    selectInTree(selected_);
    access_->setVisible(!authorized);
    items_->setVisible(authorized);
    add_->setVisible(authorized);
    reload_->setVisible(authorized);
    listStatus_->setVisible(!authorized || items_->topLevelItemCount() == 0);
    listStatus_->setText(!authorized ? QStringLiteral("Требуется подтверждение доступа.")
        : !contextAvailable_ ? QStringLiteral("Личные проекты с этим именем неоднозначны. Выберите проект коллекции в дереве.")
                             : QStringLiteral("Нет записей."));
    refreshButtons();
}

void CollectionPane::selectInTree(const std::optional<Item>& item) {
    QTreeWidgetItem* target = nullptr;
    if (item) {
        for (int index = 0; index < items_->topLevelItemCount(); ++index) {
            auto* row = items_->topLevelItem(index);
            if (row->data(0, Qt::UserRole).toString() == item->project && row->text(0) == item->name) {
                target = row;
                break;
            }
        }
    }
    const QSignalBlocker block(items_);
    items_->setCurrentItem(target);
    if (!target) items_->clearSelection();
}

void CollectionPane::editItem(const Item& item, SecureBytes value, bool enabled) {
    invalidateCopy();
    hideValue();
    editing_ = enabled;
    saved_ = item;
    savedValue_ = std::move(value);
    valueEdited_ = false;
    heading_->setText(item.name.isEmpty() ? QStringLiteral("Новая запись агента")
                                         : QStringLiteral("Агент / %1").arg(item.name));
    if (!enabled) heading_->clear();
    const QSignalBlocker blockProject(project_);
    project_->setCurrentIndex(project_->findData(item.project));
    project_->setEditText(item.project);
    const QSignalBlocker blockName(name_);
    const QSignalBlocker blockValue(value_);
    name_->setText(item.name);
    QString text;
    base64_ = !Application::collectionText(savedValue_.bytes(), text);
    const QSignalBlocker block(format_);
    format_->setCurrentIndex(base64_ ? 1 : 0);
    value_->setText(base64_ ? toBase64(savedValue_) : text);
    refreshButtons();
}

void CollectionPane::selectItem(const Item& item) {
    if (selected_ == item) return;
    discardThen([this, item] {
        SecureBytes value;
        const auto error = app_.readCollection(item.project, item.name, value);
        if (!error.isEmpty()) {
            listStatus_->setText(error);
            listStatus_->show();
            return;
        }
        selected_ = item;
        editItem(item, std::move(value));
        selectInTree(selected_);
        listStatus_->hide();
        showStatus({});
        emit selectionActivated();
    });
}

void CollectionPane::reload() {
    if (!available()) return;
    const auto previous = selected_;
    deactivate();
    refresh();
    if (previous && projects_.value(previous->project).contains(previous->name)) selectItem(*previous);
}

bool CollectionPane::readDraft(SecureBytes& value) {
    const auto text = value_->text();
    if (base64_) {
        const SecureBytes encoded(text.toUtf8());
        auto result = QByteArray::fromBase64Encoding(encoded.bytes(), QByteArray::AbortOnBase64DecodingErrors);
        value = SecureBytes(std::move(result.decoded));
        if (result) return true;
        showStatus(QStringLiteral("Введите корректное значение Base64."));
        return false;
    }
    if (!text.isValidUtf16()) {
        showStatus(QStringLiteral("Введите корректное текстовое значение."));
        return false;
    }
    value = SecureBytes(text.toUtf8());
    return true;
}

void CollectionPane::changeFormat(bool base64) {
    if (base64 == base64_) return;
    SecureBytes bytes;
    QString text;
    const bool valid = available() && readDraft(bytes);
    if (valid && (base64 || Application::collectionText(bytes.bytes(), text))) {
        base64_ = base64;
        value_->setText(base64 ? toBase64(bytes) : text);
        showStatus({});
        return;
    }
    if (valid) showStatus(QStringLiteral("Для этого значения используйте Base64."));
    const QSignalBlocker block(format_);
    format_->setCurrentIndex(base64_ ? 1 : 0);
}

void CollectionPane::save() {
    if (!available() || !dirty() || name_->text().isEmpty()) return;
    SecureBytes draft;
    if (valueEdited_ && !readDraft(draft)) return;
    const QByteArrayView value = valueEdited_ ? draft.bytes() : savedValue_.bytes();
    const Item next{project_->currentText(), name_->text()};
    const auto error = saved_.name.isEmpty()
        ? app_.copyToCollection(next.project, next.name, value)
        : app_.updateCollection(saved_.project, saved_.name, savedValue_.bytes(), next.project, next.name, value);
    showStatus(error);
    if (!error.isEmpty()) return;
    auto projects = projects_;
    if (!saved_.name.isEmpty()) {
        auto& old = projects[saved_.project];
        old.removeAll(saved_.name);
        if (old.isEmpty()) projects.remove(saved_.project);
    }
    auto& names = projects[next.project];
    names.removeAll(next.name);
    names.append(next.name);
    if (matches(next)) {
        if (!valueEdited_) draft = std::move(savedValue_);
        editItem(next, std::move(draft));
        selected_ = next;
    } else {
        deactivate();
    }
    replaceProjects(std::move(projects));
    showStatus(QStringLiteral("Запись сохранена."));
}

void CollectionPane::remove() {
    if (!available() || saved_.name.isEmpty()) return;
    const auto prompt = dirty()
        ? QStringLiteral("Удалить «%1» из раздела «%2» и отменить несохранённые изменения?")
        : QStringLiteral("Удалить «%1» из раздела «%2»?");
    beginAction(prompt.arg(saved_.name, projectLabel(saved_.project)), QStringLiteral("Удалить"),
                [this](const Item& source, const SecureBytes& original) {
        const auto error = app_.deleteCollection(source.project, source.name, original.bytes());
        showStatus(error);
        if (!error.isEmpty()) return;
        forgetSavedItem();
        showStatus(QStringLiteral("Запись удалена."));
    });
}

void CollectionPane::forgetSavedItem() {
    auto projects = projects_;
    const auto project = saved_.project;
    auto& names = projects[project];
    names.removeAll(saved_.name);
    if (names.isEmpty()) projects.remove(project);
    deactivate();
    replaceProjects(std::move(projects));
}

void CollectionPane::transfer(bool move) {
    if (!available() || pending_ || saved_.name.isEmpty()) return;
    auto message = move
        ? QStringLiteral("Переместить «%1» из раздела «%2». Исходная запись будет удалена после сохранения копии.")
        : QStringLiteral("Копировать «%1» из раздела «%2» в хранилище.");
    message = message.arg(saved_.name, projectLabel(saved_.project));
    if (dirty()) {
        message += move
            ? QStringLiteral(" При успешном перемещении несохранённые изменения будут сброшены.")
            : QStringLiteral(" Будет скопирована сохранённая версия.");
    }
    destination_->clear();
    QMap<QString,QList<model::Id>> paths;
    QHash<model::Id,QList<model::VaultModel::Node>> destinations;
    QList<model::Id> matchingRoots;
    const auto& nodes = app_.model()->nodes();
    for (const auto& node : nodes) {
        if (node.parent.isNull() && node.name == saved_.project) matchingRoots.append(node.id);
        auto path = vaultPath(*app_.model(), node.id);
        QStringList pieces;
        for (const auto& piece : path) pieces.append(piece.name);
        destinations.insert(node.id, std::move(path));
        paths[pieces.join(QStringLiteral(" / "))].append(node.id);
    }
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        auto ids = it.value();
        std::sort(ids.begin(), ids.end());
        for (const auto id : ids) {
            const auto label = ids.size() == 1 ? it.key()
                : QStringLiteral("%1 [%2]").arg(it.key(), id.toString(QUuid::WithoutBraces));
            destination_->addItem(label, id);
        }
    }
    destination_->addItem(QStringLiteral("Новый проект…"));
    int preferred = vaultDestination_.isNull() ? -1 : destination_->findData(vaultDestination_);
    if (preferred < 0 && matchingRoots.size() == 1) preferred = destination_->findData(matchingRoots.first());
    destination_->setCurrentIndex(nodes.isEmpty() ? 0 : preferred);
    newProject_->setText(matchingRoots.isEmpty() ? saved_.project : QString());
    beginAction(message, move ? QStringLiteral("Переместить") : QStringLiteral("Копировать"),
        [this, move, destinations = std::move(destinations)](const Item& source, const SecureBytes& original) {
        const auto id = destination_->currentData().value<model::Id>();
        const auto name = id.isNull() ? newProject_->text().trimmed() : QString();
        const auto destinationStill = [&] {
            if (!id.isNull()) return vaultPath(*app_.model(), id) == destinations.value(id);
            const auto& nodes = app_.model()->nodes();
            return std::none_of(nodes.cbegin(), nodes.cend(), [&name](const auto& node) {
                return node.parent.isNull() && node.name == name;
            });
        };
        const auto changed = QStringLiteral("Назначение изменилось или уже существует. Выберите проект или группу заново.");
        if (!destinationStill()) { showStatus(changed); return; }
        if (!approveUpgrade_(this) || !available() || !sourceStill(source, original)) return;
        if (!destinationStill()) { showStatus(changed); return; }
        const auto error = app_.transferFromCollection(source.project, source.name, original.bytes(), id, name, move);
        if (!sourceStill(source, original)) return;
        showStatus(error);
        if (!error.isEmpty()) return;
        if (move) forgetSavedItem();
        showStatus(move ? QStringLiteral("Запись перемещена в хранилище.") : QStringLiteral("Копия сохранена в хранилище."));
        emit vaultChanged();
    }, true);
}
}
