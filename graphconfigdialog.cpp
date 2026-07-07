/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphconfigdialog.h"

#include "graphsettingsbase.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

GraphConfigDialog::GraphConfigDialog(GraphSettings *settings, const QString &resourceName, QWidget *parent)
    : QDialog(parent)
    , mSettings(settings)
{
    setWindowTitle(i18nc("@title:window", "Microsoft 365 (Graph) Account"));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    mName = new QLineEdit(resourceName, this);
    form->addRow(i18nc("@label:textbox", "Account name:"), mName);

    mEmail = new QLineEdit(mSettings->email(), this);
    mEmail->setPlaceholderText(QStringLiteral("user@example.com"));
    form->addRow(i18nc("@label:textbox", "Email / UPN:"), mEmail);

    mTenant = new QLineEdit(mSettings->tenantId(), this);
    mTenant->setPlaceholderText(QStringLiteral("common"));
    form->addRow(i18nc("@label:textbox", "Azure tenant:"), mTenant);

    mClientId = new QLineEdit(mSettings->clientId(), this);
    form->addRow(i18nc("@label:textbox", "Application (client) ID:"), mClientId);

    mPollInterval = new QSpinBox(this);
    mPollInterval->setRange(1, 1440);
    mPollInterval->setSuffix(i18nc("@item:valuesuffix minutes", " min"));
    mPollInterval->setValue(mSettings->pollInterval() > 0 ? mSettings->pollInterval() : 5);
    form->addRow(i18nc("@label:spinbox", "Check for new mail every:"), mPollInterval);

    auto *hint = new QLabel(i18nc("@info", "Changing the tenant or client id requires signing in again."), this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &GraphConfigDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &GraphConfigDialog::reject);
    layout->addWidget(buttons);
}

QString GraphConfigDialog::displayName() const
{
    return mName->text();
}

void GraphConfigDialog::accept()
{
    mSettings->setEmail(mEmail->text().trimmed());
    mSettings->setTenantId(mTenant->text().trimmed().isEmpty() ? QStringLiteral("common") : mTenant->text().trimmed());
    mSettings->setClientId(mClientId->text().trimmed());
    mSettings->setPollInterval(mPollInterval->value());
    mSettings->save();
    QDialog::accept();
}

#include "moc_graphconfigdialog.cpp"
