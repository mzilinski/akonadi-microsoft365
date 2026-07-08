/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Account configuration dialog: display name, primary email/UPN, Azure tenant and
    client id, and the delta poll interval. Built in code (no .ui) to keep it small.
*/

#pragma once

#include <QDialog>

class GraphSettings;
class QLineEdit;
class QSpinBox;

class GraphConfigDialog : public QDialog
{
    Q_OBJECT
public:
    GraphConfigDialog(GraphSettings *settings, const QString &resourceName, QWidget *parent = nullptr);

    /// The name the user chose for the resource instance (may be empty = unchanged).
    [[nodiscard]] QString displayName() const;

    void accept() override;

private:
    GraphSettings *const mSettings;
    QLineEdit *const mName;
    QLineEdit *const mEmail;
    QLineEdit *const mTenant;
    QLineEdit *const mClientId;
    QSpinBox *const mPollInterval;
};
