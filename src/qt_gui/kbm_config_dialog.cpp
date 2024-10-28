// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "kbm_config_dialog.h"
#include "kbm_help_dialog.h"

#include "src/sdl_window.h"
#include "common/path_util.h"
#include <iostream>
#include <fstream>

#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QCloseEvent>

EditorDialog::EditorDialog(QWidget *parent)
        : QDialog(parent) {


    setWindowTitle("Edit Config File");
    resize(600, 400);

    // Create the editor widget
    editor = new QPlainTextEdit(this);
    editorFont.setPointSize(10);  // Set default text size
    editor->setFont(editorFont);  // Apply font to the editor

    // Create Save, Cancel, and Help buttons
    QPushButton *saveButton = new QPushButton("Save", this);
    QPushButton *cancelButton = new QPushButton("Cancel", this);
    QPushButton *helpButton = new QPushButton("Help", this);

    // Create layout for buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(saveButton);
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(helpButton);

    // Main layout with editor and buttons
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(editor);
    layout->addLayout(buttonLayout);

    // Load the INI file content into the editor
    loadFile();
    

    // Connect the Save button
    connect(saveButton, &QPushButton::clicked, this, &EditorDialog::onSaveClicked);

    // Connect the Cancel button
    connect(cancelButton, &QPushButton::clicked, this, &EditorDialog::onCancelClicked);

    // Connect the Help button
    connect(helpButton, &QPushButton::clicked, this, &EditorDialog::onHelpClicked);
}

void EditorDialog::loadFile() {
    const auto config_file = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "kbmConfig" / "default.ini";
    if (!std::filesystem::exists(config_file)) {
        // create it
        std::ofstream file;
        file.open(config_file, std::ios::out);
        if (file.is_open()) {
            file << KBMConfig::getDefaultKeyboardConfig();
            file.close();
        } else {
            QMessageBox::warning(this, "Error", "Could not create the file");
        }
    }
    QFile file(config_file);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        editor->setPlainText(in.readAll());
        originalConfig = editor->toPlainText();
        file.close();
    } else {
        QMessageBox::warning(this, "Error", "Could not open the file for reading");
    }
}

void EditorDialog::saveFile() {
    const auto config_file = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "kbmConfig" / "default.ini";
    QFile file(config_file);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << editor->toPlainText();
        file.close();
    } else {
        QMessageBox::warning(this, "Error", "Could not open the file for writing");
    }
}

// Override the close event to show the save confirmation dialog only if changes were made
void EditorDialog::closeEvent(QCloseEvent *event) {
    if (hasUnsavedChanges()) {
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "Save Changes",
                                      "Do you want to save changes?",
                                      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

        if (reply == QMessageBox::Yes) {
            saveFile();
            event->accept();  // Close the dialog
        } else if (reply == QMessageBox::No) {
            event->accept();  // Close the dialog without saving
        } else {
            event->ignore();  // Cancel the close event
        }
    } else {
        event->accept();  // No changes, close the dialog without prompting
    }
}
void EditorDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        close();  // Trigger the close action, same as pressing the close button
    } else {
        QDialog::keyPressEvent(event);  // Call the base class implementation for other keys
    }
}

void EditorDialog::onSaveClicked() {
    saveFile();
    reject();  // Close the dialog
}

void EditorDialog::onCancelClicked() {
    reject();  // Close the dialog
}

void EditorDialog::onHelpClicked() {

    HelpDialog *helpDialog = new HelpDialog(this);
    helpDialog->setWindowTitle("Help");
    helpDialog->exec();
}

bool EditorDialog::hasUnsavedChanges() {
    // Compare the current content with the original content to check if there are unsaved changes
    return editor->toPlainText() != originalConfig;
}
