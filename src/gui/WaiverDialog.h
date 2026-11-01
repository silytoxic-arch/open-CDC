#pragma once

#include "cdc/CdcModel.h"

#include <QDialog>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace opencdc::gui {

/// Collects a waiver for one rule hit or output crossing: id, reason, author,
/// expiry date and the waiver file it goes to. The selectors (rule,
/// fingerprint) come from what is waived and are shown read-only.
class WaiverDialog final : public QDialog {
    Q_OBJECT

public:
    /// `files`: the project's waiver files (the first is preselected); the
    /// last entry of the combo box is `newFile` when it is not among them.
    WaiverDialog(cdc::WaiverRecord proposal, const QString& what, const QStringList& files, const QString& newFile,
                 const std::vector<std::string>& takenIds, QWidget* parent = nullptr);

    /// The waiver as edited (selectors from the proposal).
    [[nodiscard]] cdc::WaiverRecord waiver() const;
    [[nodiscard]] QString file() const;
    /// Empty if the waiver can be written, otherwise what is wrong.
    [[nodiscard]] QString validationError() const;

    // Programmatic editing (headless self-test).
    void setReason(const QString& reason);
    void setExpires(const QString& date);
    void setId(const QString& id);

public slots:
    void accept() override;

private:
    cdc::WaiverRecord proposal_;
    std::vector<std::string> taken_;
    QLineEdit* id_;
    QPlainTextEdit* reason_;
    QLineEdit* author_;
    QLineEdit* expires_;
    QComboBox* file_;
};

} // namespace opencdc::gui
