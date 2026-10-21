#include "gui/ProjectEditor.h"

#include "core/PathUtil.h"
#include "frontend/Frontend.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace opencdc::gui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
QString qs(const std::filesystem::path& p) { return QString::fromStdString(toUtf8(p)); }
std::filesystem::path toPath(const QString& s) { return fromUtf8(s.toStdString()).lexically_normal(); }

QPushButton* button(const QString& text, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setAutoDefault(false);
    return b;
}

/// A widget with `content` on the left and a column of buttons on the right.
QWidget* withButtons(QWidget* content, const std::vector<QPushButton*>& buttons, QWidget* parent) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(content, 1);
    auto* v = new QVBoxLayout;
    for (auto* b : buttons)
        v->addWidget(b);
    v->addStretch(1);
    h->addLayout(v);
    return w;
}

QTableWidget* makeTable(const QStringList& headers, QWidget* parent) {
    auto* t = new QTableWidget(0, static_cast<int>(headers.size()), parent);
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setStretchLastSection(true);
    t->verticalHeader()->setVisible(false);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    return t;
}

int selectedRow(QTableWidget* t) {
    const auto rows = t->selectionModel() ? t->selectionModel()->selectedRows() : QModelIndexList{};
    return rows.isEmpty() ? t->currentRow() : rows.first().row();
}

void setRow(QTableWidget* t, int row, const QString& a, const QString& b) {
    t->setItem(row, 0, new QTableWidgetItem(a));
    t->setItem(row, 1, new QTableWidgetItem(b));
}

QString cell(const QTableWidget* t, int row, int col) {
    const auto* item = t->item(row, col);
    return item ? item->text() : QString();
}

std::vector<std::filesystem::path> listPaths(const QListWidget* list) {
    std::vector<std::filesystem::path> out;
    for (int i = 0; i < list->count(); ++i)
        out.push_back(toPath(list->item(i)->text()));
    return out;
}

} // namespace

ProjectEditor::ProjectEditor(const engine::ProjectConfig& config, const QString& baseDir, QWidget* parent)
    : QDialog(parent),
      original_(config),
      baseDir_(baseDir),
      name_(new QLineEdit(qs(config.name), this)),
      top_(new QLineEdit(qs(config.top), this)),
      language_(new QComboBox(this)),
      sources_(makeTable({tr("File"), tr("VHDL library")}, this)),
      includeDirs_(nullptr),
      defines_(makeTable({tr("Macro"), tr("Value")}, this)),
      parameters_(makeTable({tr("Parameter / generic"), tr("Value (HDL syntax)")}, this)),
      constraints_(nullptr),
      waivers_(nullptr),
      unknownClock_(new QComboBox(this)),
      minStages_(new QSpinBox(this)),
      reconvDepth_(new QSpinBox(this)) {
    setWindowTitle(tr("Project Settings"));
    resize(820, 560);

    for (auto lang : {frontend::LanguageStd::Verilog2001, frontend::LanguageStd::Verilog2005,
                      frontend::LanguageStd::SystemVerilog2017})
        language_->addItem(QString::fromLatin1(frontend::toString(lang).data()), static_cast<int>(lang));
    language_->setCurrentIndex(language_->findData(static_cast<int>(config.language)));
    top_->setPlaceholderText(tr("inferred when empty"));

    auto* tabs = new QTabWidget(this);

    // General
    auto* general = new QWidget(tabs);
    auto* form = new QFormLayout(general);
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("Top module / entity:"), top_);
    form->addRow(tr("Verilog standard:"), language_);
    unknownClock_->addItem(tr("Report (flag for review)"), static_cast<int>(engine::UnknownClockPolicy::Report));
    unknownClock_->addItem(tr("Assume asynchronous"), static_cast<int>(engine::UnknownClockPolicy::AssumeAsync));
    unknownClock_->addItem(tr("Assume synchronous"), static_cast<int>(engine::UnknownClockPolicy::AssumeSync));
    unknownClock_->setCurrentIndex(unknownClock_->findData(static_cast<int>(config.options.unknownClockRelation)));
    minStages_->setRange(1, 8);
    minStages_->setValue(static_cast<int>(config.options.minSyncStages));
    reconvDepth_->setRange(0, 8);
    reconvDepth_->setValue(static_cast<int>(config.options.reconvergenceDepth));
    form->addRow(tr("Unknown clock relation:"), unknownClock_);
    form->addRow(tr("Minimum synchronizer stages:"), minStages_);
    form->addRow(tr("Reconvergence depth:"), reconvDepth_);
    tabs->addTab(general, tr("General"));

    // Sources
    for (std::size_t i = 0; i < config.sources.size(); ++i) {
        const int row = sources_->rowCount();
        sources_->insertRow(row);
        setRow(sources_, row, qs(config.sources[i]),
               i < config.sourceLibraries.size() ? qs(config.sourceLibraries[i]) : QString());
        sources_->item(row, 0)->setFlags(sources_->item(row, 0)->flags() & ~Qt::ItemIsEditable);
    }
    sources_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    sources_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    auto* addSrc = button(tr("Add..."), this);
    auto* rmSrc = button(tr("Remove"), this);
    auto* upSrc = button(tr("Up"), this);
    auto* downSrc = button(tr("Down"), this);
    connect(addSrc, &QPushButton::clicked, this, [this] {
        addSources(QFileDialog::getOpenFileNames(
            this, tr("Add HDL sources"), baseDir_,
            tr("HDL sources (*.v *.vh *.sv *.svh *.vhd *.vhdl);;All files (*)")));
    });
    connect(rmSrc, &QPushButton::clicked, this, [this] { removeSource(selectedRow(sources_)); });
    connect(upSrc, &QPushButton::clicked, this, [this] { moveSource(selectedRow(sources_), -1); });
    connect(downSrc, &QPushButton::clicked, this, [this] { moveSource(selectedRow(sources_), +1); });
    auto* srcPage = new QWidget(tabs);
    auto* srcLayout = new QVBoxLayout(srcPage);
    srcLayout->addWidget(withButtons(sources_, {addSrc, rmSrc, upSrc, downSrc}, srcPage));
    auto* hint = new QLabel(tr("Order matters for Verilog compilation units. The library column applies to "
                               "VHDL files (empty = work)."),
                            srcPage);
    hint->setWordWrap(true);
    srcLayout->addWidget(hint);
    tabs->addTab(srcPage, tr("Sources"));

    // Constraints and waivers
    constraints_ = makePathList(config.constraints);
    waivers_ = makePathList(config.waivers);
    auto* addCon = button(tr("Add..."), this);
    auto* rmCon = button(tr("Remove"), this);
    connect(addCon, &QPushButton::clicked, this, [this] {
        addConstraints(QFileDialog::getOpenFileNames(this, tr("Add constraints"), baseDir_,
                                                     tr("Constraints (*.sdc *.xdc);;All files (*)")));
    });
    connect(rmCon, &QPushButton::clicked, this, [this] { delete constraints_->currentItem(); });
    auto* addWv = button(tr("Add..."), this);
    auto* rmWv = button(tr("Remove"), this);
    connect(addWv, &QPushButton::clicked, this, [this] {
        addWaivers(QFileDialog::getOpenFileNames(this, tr("Add waiver files"), baseDir_,
                                                 tr("Waivers (*.json);;All files (*)")));
    });
    connect(rmWv, &QPushButton::clicked, this, [this] { delete waivers_->currentItem(); });
    auto* conPage = new QWidget(tabs);
    auto* conLayout = new QVBoxLayout(conPage);
    conLayout->addWidget(new QLabel(tr("SDC / XDC constraints:"), conPage));
    conLayout->addWidget(withButtons(constraints_, {addCon, rmCon}, conPage), 2);
    conLayout->addWidget(new QLabel(tr("Waiver files:"), conPage));
    conLayout->addWidget(withButtons(waivers_, {addWv, rmWv}, conPage), 1);
    tabs->addTab(conPage, tr("Constraints && Waivers"));

    // Preprocessor and parameters
    includeDirs_ = makePathList(config.includeDirs);
    for (const auto& d : config.defines)
        setDefine(qs(d.name), d.value ? qs(*d.value) : QString(), d.value.has_value());
    for (const auto& [n, v] : config.parameters)
        setParameter(qs(n), qs(v));
    auto* addInc = button(tr("Add..."), this);
    auto* rmInc = button(tr("Remove"), this);
    connect(addInc, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Add include directory"), baseDir_);
        if (!dir.isEmpty())
            addIncludeDir(dir);
    });
    connect(rmInc, &QPushButton::clicked, this, [this] { delete includeDirs_->currentItem(); });
    auto* addDef = button(tr("Add"), this);
    auto* rmDef = button(tr("Remove"), this);
    connect(addDef, &QPushButton::clicked, this, [this] {
        defines_->insertRow(defines_->rowCount());
        setRow(defines_, defines_->rowCount() - 1, QStringLiteral("NAME"), QString());
        defines_->editItem(defines_->item(defines_->rowCount() - 1, 0));
    });
    connect(rmDef, &QPushButton::clicked, this, [this] {
        if (const int r = selectedRow(defines_); r >= 0)
            defines_->removeRow(r);
    });
    auto* addPar = button(tr("Add"), this);
    auto* rmPar = button(tr("Remove"), this);
    connect(addPar, &QPushButton::clicked, this, [this] {
        parameters_->insertRow(parameters_->rowCount());
        setRow(parameters_, parameters_->rowCount() - 1, QStringLiteral("NAME"), QStringLiteral("0"));
        parameters_->editItem(parameters_->item(parameters_->rowCount() - 1, 0));
    });
    connect(rmPar, &QPushButton::clicked, this, [this] {
        if (const int r = selectedRow(parameters_); r >= 0)
            parameters_->removeRow(r);
    });
    auto* ppPage = new QWidget(tabs);
    auto* ppLayout = new QVBoxLayout(ppPage);
    ppLayout->addWidget(new QLabel(tr("Top-level parameter / generic overrides:"), ppPage));
    ppLayout->addWidget(withButtons(parameters_, {addPar, rmPar}, ppPage), 1);
    ppLayout->addWidget(new QLabel(tr("Verilog include directories:"), ppPage));
    ppLayout->addWidget(withButtons(includeDirs_, {addInc, rmInc}, ppPage), 1);
    ppLayout->addWidget(new QLabel(tr("Verilog defines (an empty value defines the macro without a value):"),
                                   ppPage));
    ppLayout->addWidget(withButtons(defines_, {addDef, rmDef}, ppPage), 1);
    tabs->addTab(ppPage, tr("Parameters && Defines"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProjectEditor::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);
}

QListWidget* ProjectEditor::makePathList(const std::vector<std::filesystem::path>& paths) {
    auto* list = new QListWidget(this);
    for (const auto& p : paths)
        list->addItem(qs(p));
    return list;
}

void ProjectEditor::addPathItems(QListWidget* list, const QStringList& paths) {
    for (const auto& p : paths) {
        const QString norm = qs(toPath(p));
        if (list->findItems(norm, Qt::MatchExactly).isEmpty())
            list->addItem(norm);
    }
}

void ProjectEditor::addSources(const QStringList& files) {
    for (const auto& f : files) {
        const QString norm = qs(toPath(f));
        bool present = false;
        for (int r = 0; r < sources_->rowCount() && !present; ++r)
            present = cell(sources_, r, 0) == norm;
        if (present)
            continue;
        const int row = sources_->rowCount();
        sources_->insertRow(row);
        setRow(sources_, row, norm, QString());
        sources_->item(row, 0)->setFlags(sources_->item(row, 0)->flags() & ~Qt::ItemIsEditable);
    }
}

void ProjectEditor::removeSource(int row) {
    if (row >= 0 && row < sources_->rowCount())
        sources_->removeRow(row);
}

void ProjectEditor::moveSource(int row, int delta) {
    const int to = row + delta;
    if (row < 0 || row >= sources_->rowCount() || to < 0 || to >= sources_->rowCount())
        return;
    const QString f = cell(sources_, row, 0);
    const QString l = cell(sources_, row, 1);
    sources_->removeRow(row);
    sources_->insertRow(to);
    setRow(sources_, to, f, l);
    sources_->item(to, 0)->setFlags(sources_->item(to, 0)->flags() & ~Qt::ItemIsEditable);
    sources_->selectRow(to);
}

void ProjectEditor::setSourceLibrary(int row, const QString& library) {
    if (row >= 0 && row < sources_->rowCount())
        sources_->setItem(row, 1, new QTableWidgetItem(library));
}

void ProjectEditor::addConstraints(const QStringList& files) { addPathItems(constraints_, files); }
void ProjectEditor::addIncludeDir(const QString& dir) { addPathItems(includeDirs_, {dir}); }
void ProjectEditor::addWaivers(const QStringList& files) { addPathItems(waivers_, files); }

void ProjectEditor::setParameter(const QString& name, const QString& value) {
    for (int r = 0; r < parameters_->rowCount(); ++r)
        if (cell(parameters_, r, 0) == name) {
            parameters_->setItem(r, 1, new QTableWidgetItem(value));
            return;
        }
    parameters_->insertRow(parameters_->rowCount());
    setRow(parameters_, parameters_->rowCount() - 1, name, value);
}

void ProjectEditor::setDefine(const QString& name, const QString& value, bool hasValue) {
    const QString v = hasValue ? value : QString();
    for (int r = 0; r < defines_->rowCount(); ++r)
        if (cell(defines_, r, 0) == name) {
            defines_->setItem(r, 1, new QTableWidgetItem(v));
            return;
        }
    defines_->insertRow(defines_->rowCount());
    setRow(defines_, defines_->rowCount() - 1, name, v);
}

engine::ProjectConfig ProjectEditor::config() const {
    engine::ProjectConfig c;
    c.projectFile = original_.projectFile;
    c.name = name_->text().trimmed().toStdString();
    c.top = top_->text().trimmed().toStdString();
    c.language = static_cast<frontend::LanguageStd>(language_->currentData().toInt());
    for (int r = 0; r < sources_->rowCount(); ++r) {
        c.sources.push_back(toPath(cell(sources_, r, 0)));
        c.sourceLibraries.push_back(cell(sources_, r, 1).trimmed().toStdString());
    }
    c.includeDirs = listPaths(includeDirs_);
    for (int r = 0; r < defines_->rowCount(); ++r) {
        const QString n = cell(defines_, r, 0).trimmed();
        if (n.isEmpty())
            continue;
        const QString v = cell(defines_, r, 1);
        c.defines.push_back({n.toStdString(), v.isEmpty() ? std::nullopt : std::optional(v.toStdString())});
    }
    for (int r = 0; r < parameters_->rowCount(); ++r) {
        const QString n = cell(parameters_, r, 0).trimmed();
        if (!n.isEmpty())
            c.parameters.emplace_back(n.toStdString(), cell(parameters_, r, 1).trimmed().toStdString());
    }
    c.constraints = listPaths(constraints_);
    c.waivers = listPaths(waivers_);
    c.options.unknownClockRelation = static_cast<engine::UnknownClockPolicy>(unknownClock_->currentData().toInt());
    c.options.minSyncStages = static_cast<std::uint32_t>(minStages_->value());
    c.options.reconvergenceDepth = static_cast<std::uint32_t>(reconvDepth_->value());
    return c;
}

QString ProjectEditor::validationError() const {
    if (name_->text().trimmed().isEmpty())
        return tr("The project needs a name.");
    if (sources_->rowCount() == 0)
        return tr("The project needs at least one source file.");
    for (int r = 0; r < parameters_->rowCount(); ++r)
        if (!cell(parameters_, r, 0).trimmed().isEmpty() && cell(parameters_, r, 1).trimmed().isEmpty())
            return tr("Parameter '%1' has no value.").arg(cell(parameters_, r, 0).trimmed());
    return {};
}

void ProjectEditor::accept() {
    if (const QString err = validationError(); !err.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), err);
        return;
    }
    QDialog::accept();
}

} // namespace opencdc::gui
