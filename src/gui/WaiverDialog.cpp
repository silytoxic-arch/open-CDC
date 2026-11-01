#include "gui/WaiverDialog.h"

#include "engine/WaiverFile.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace opencdc::gui {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }

QString userName() {
    for (const char* var : {"USER", "USERNAME"}) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty())
            return v;
    }
    return {};
}
} // namespace

WaiverDialog::WaiverDialog(cdc::WaiverRecord proposal, const QString& what, const QStringList& files,
                           const QString& newFile, const std::vector<std::string>& takenIds, QWidget* parent)
    : QDialog(parent), proposal_(std::move(proposal)), taken_(takenIds),
      id_(new QLineEdit(qs(proposal_.id.empty() ? engine::nextWaiverId(takenIds) : proposal_.id), this)),
      reason_(new QPlainTextEdit(this)), author_(new QLineEdit(userName(), this)), expires_(new QLineEdit(this)),
      file_(new QComboBox(this)) {
    setWindowTitle(tr("Add Waiver"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(what, this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    auto* rule = new QLabel(qs(proposal_.rule), this);
    auto* fp = new QLabel(qs(proposal_.fingerprint), this);
    rule->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fp->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Rule:"), rule);
    form->addRow(tr("Fingerprint:"), fp);
    form->addRow(tr("Id:"), id_);
    reason_->setPlaceholderText(tr("Why this is acceptable (required)"));
    reason_->setTabChangesFocus(true);
    reason_->setMaximumHeight(90);
    form->addRow(tr("Reason:"), reason_);
    form->addRow(tr("Author:"), author_);
    expires_->setPlaceholderText(tr("YYYY-MM-DD (optional)"));
    form->addRow(tr("Expires:"), expires_);
    for (const auto& f : files)
        file_->addItem(f, f);
    if (!newFile.isEmpty() && !files.contains(newFile))
        file_->addItem(tr("%1 (new, added to the project)").arg(newFile), newFile);
    form->addRow(tr("Waiver file:"), file_);
    layout->addLayout(form);
    auto* note = new QLabel(tr("The waiver covers this rule on this path only (by fingerprint): a design change that "
                               "creates another crossing is not waived silently."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &WaiverDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(560, sizeHint().height());
}

cdc::WaiverRecord WaiverDialog::waiver() const {
    cdc::WaiverRecord w = proposal_;
    w.id = id_->text().trimmed().toStdString();
    w.reason = reason_->toPlainText().simplified().toStdString();
    w.author = author_->text().trimmed().toStdString();
    w.expires = expires_->text().trimmed().toStdString();
    return w;
}

QString WaiverDialog::file() const {
    return file_->currentData().toString();
}

QString WaiverDialog::validationError() const {
    if (file().isEmpty())
        return tr("Choose a waiver file.");
    return qs(engine::waiverProblem(waiver(), taken_));
}

void WaiverDialog::setReason(const QString& reason) { reason_->setPlainText(reason); }
void WaiverDialog::setExpires(const QString& date) { expires_->setText(date); }
void WaiverDialog::setId(const QString& id) { id_->setText(id); }

void WaiverDialog::accept() {
    if (const QString err = validationError(); !err.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), err);
        return;
    }
    QDialog::accept();
}

} // namespace opencdc::gui
