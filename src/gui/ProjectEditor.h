#pragma once

#include "engine/ProjectConfig.h"

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;

namespace opencdc::gui {

/// Dialog that edits a project: name, top, language, sources with their VHDL
/// library, include directories, defines, top parameters / generics,
/// constraints, waivers and the analysis options. It only edits an
/// engine::ProjectConfig; saving is up to the caller.
class ProjectEditor final : public QDialog {
    Q_OBJECT

public:
    /// `baseDir` is where file dialogs start (the project directory).
    explicit ProjectEditor(const engine::ProjectConfig& config, const QString& baseDir, QWidget* parent = nullptr);

    /// The configuration as currently edited (projectFile is kept from the input).
    [[nodiscard]] engine::ProjectConfig config() const;
    /// Empty if the configuration can be used, otherwise what is wrong.
    [[nodiscard]] QString validationError() const;

    // Programmatic editing (used by the buttons and by the headless self-test).
    void addSources(const QStringList& files);
    void removeSource(int row);
    void moveSource(int row, int delta);
    void setSourceLibrary(int row, const QString& library);
    void addConstraints(const QStringList& files);
    void addIncludeDir(const QString& dir);
    void addWaivers(const QStringList& files);
    void setParameter(const QString& name, const QString& value);
    void setDefine(const QString& name, const QString& value, bool hasValue);

    [[nodiscard]] QTableWidget* sourceTable() const { return sources_; }
    [[nodiscard]] QListWidget* constraintList() const { return constraints_; }

public slots:
    void accept() override;

private:
    QListWidget* makePathList(const std::vector<std::filesystem::path>& paths);
    void addPathItems(QListWidget* list, const QStringList& paths);

    engine::ProjectConfig original_;
    QString baseDir_;
    QLineEdit* name_;
    QLineEdit* top_;
    QComboBox* language_;
    QTableWidget* sources_;
    QListWidget* includeDirs_;
    QTableWidget* defines_;
    QTableWidget* parameters_;
    QListWidget* constraints_;
    QListWidget* waivers_;
    QComboBox* unknownClock_;
    QSpinBox* minStages_;
    QSpinBox* reconvDepth_;
};

} // namespace opencdc::gui
