/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Quick 3D.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Qt.labs.settings

import QtQuick3D
import MaterialEditorHelpers 1.0

ApplicationWindow {
    id: window
    height: 720
    width: 1024
    visible: true
    title: qsTr("Material Editor")

    Settings {
        id: settings
        property alias windowX: window.x
        property alias windowY: window.y
        property alias windowWidth: window.width
        property alias windowHeight: window.height
        property alias windowVisibility: window.visibility
    }

    Component.onCompleted: {
        mainSplitView.restoreState(settings.value("ui/mainSplitView"))
        editorView.restoreState(settings.value("ui/editorView"))
    }
    Component.onDestruction: {
        settings.setValue("ui/mainSplitView", mainSplitView.saveState())
        settings.setValue("ui/editorView", editorView.saveState())
    }

    QtObject {
        id: resourceStore
        objectName: "QtQuick3DResourceStorePrivate"
    }

    FileDialog {
        id: openMaterialDialog
        title: "Open a Material Project File"
        nameFilters: [ "Material Editor Project (*.qmp)"]
        onAccepted: {
            if (openMaterialDialog.selectedFile != null)
                materialAdapter.loadMaterial(openMaterialDialog.selectedFile);
        }
    }

    FileDialog {
        id: saveAsDialog
        fileMode: FileDialog.SaveFile
        nameFilters: [ "Material Editor Project (*.qmp)"]
        onAccepted: materialAdapter.saveMaterial(selectedFile)

    }


    FileDialog {
        id: fragmentShaderImportDialog
        title: "Fragment Shader to import"
        nameFilters: [ "Fragment Shader (*.frag *.fs *.glsl)" ]
        onAccepted: {
            if (fragmentShaderImportDialog.selectedFile != null) {
                materialAdapter.importFragmentShader(fragmentShaderImportDialog.selectedFile)
            }
        }
    }

    FileDialog {
        id: vertexShaderImportDialog
        title: "Vertex Shader to import"
        nameFilters: [ "Vertex Shader (*.vert *.vs *.glsl)" ]
        onAccepted: {
            if (vertexShaderImportDialog.selectedFile != null) {
                materialAdapter.importVertexShader(vertexShaderImportDialog.selectedFile)
            }
        }
    }

    FileDialog {
        id: saveCompFileDialog
        title: "Choose file"
        nameFilters: [ "QML Componen (*.qml)" ]
        fileMode: FileDialog.SaveFile
        onAccepted: {
            if (selectedFile != null)
                componentFilePath.text = selectedFile
        }
    }

    RegularExpressionValidator {
        id: nameValidator
        regularExpression: /[a-zA-Z0-9_-]*/
    }

    Dialog {
        id: exportMaterialDialog
        title: "Export material"
        anchors.centerIn: parent

        ColumnLayout {
            id: exportFiles
            anchors.fill: parent
            spacing: 1
            RowLayout {
                Text {
                    text: qsTr("Component")
                    color: palette.text
                }
                TextField {
                    id: componentFilePath
                    readOnly: true
                }
                Button {
                    text: qsTr("Choose...")
                    onClicked: {
                        saveCompFileDialog.open()
                        exportMaterialDialog.aboutToHide()
                    }
                }
            }
            RowLayout {
                Text {
                    text: qsTr("Vertex:")
                    color: palette.text
                }
                TextField {
                    id: vertexFilename
                    enabled: (editorView.vertexEditor.text !== "")
                    validator: nameValidator
                }
            }
            RowLayout {
                Text {
                    text: qsTr("Fragment:")
                    color: palette.text
                }
                TextField {
                    id: fragmentFilename
                    enabled: (editorView.fragmentEditor.text !== "")
                    validator: nameValidator
                }
            }

            DialogButtonBox {
                Button {
                    text: qsTr("Export")
                    enabled: (componentFilePath.text !== "" && (!vertexFilename.enabled || (vertexFilename.enabled && vertexFilename.text !== "")) && (!fragmentFilename.enabled || (fragmentFilename.enabled && fragmentFilename.text !== "")))
                    DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                    onClicked: exportMaterialDialog.accept()
                }
                Button {
                    text: qsTr("Cancel")
                    DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                    onClicked: exportMaterialDialog.reject()
                }
            }
        }

        onAccepted: {
            materialAdapter.exportQmlComponent(componentFilePath.text, vertexFilename.text, fragmentFilename.text)
        }
    }

    SaveChangesDialog {
        id: saveChangesDialog
        materialAdapter: materialAdapter
        saveAsDialog: saveAsDialog
        anchors.centerIn: parent
    }

    AboutDialog {
        id: aboutDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    function saveAction() {
        // 1. No file name(s) given (call saveAs)
        if (materialAdapter.materialSaveFile.toString().length > 0)
            materialAdapter.save()
        else
            saveAsAction()
    }
    function openAction() {
        openMaterialDialog.open()
    }
    function newAction() {
        saveChangesDialog.doIfChangesSavedOrDiscarded(() => { materialAdapter.reset() });
        materialAdapter.reset()
    }
    function saveAsAction() {
        saveAsDialog.open()
    }
    function quitAction() {
        Qt.quit()
    }
    function aboutAction() {
        aboutDialog.open()
    }

    function importFragmentShader() {
        fragmentShaderImportDialog.open()
    }

    function importVertexShader() {
        vertexShaderImportDialog.open()
    }

    function exportMaterial() {
        exportMaterialDialog.open()
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            Action { text: qsTr("&New..."); onTriggered: newAction(); }
            Action { text: qsTr("&Open..."); onTriggered: openAction(); }
            Action { text: qsTr("&Save"); onTriggered: saveAction(); }
            Action { text: qsTr("Save &As..."); onTriggered: saveAsAction(); }
            MenuSeparator { }
            Menu {
                title: qsTr("Import")
                Action { text: qsTr("Fragment Shader"); onTriggered: importFragmentShader(); }
                Action { text: qsTr("Vertex Shader"); onTriggered: importVertexShader(); }
            }
            Action { text: qsTr("Export"); onTriggered: exportMaterial(); }

            MenuSeparator { }
            Action { text: qsTr("&Quit"); onTriggered: quitAction(); }
        }
        Menu {
            title: qsTr("&Help")
            Action { text: qsTr("&About"); onTriggered: aboutAction(); }
        }
    }

    SplitView {
        id: mainSplitView
        anchors.fill: parent
        orientation: Qt.Horizontal
        EditorView {
            id: editorView
            uniformTable2: materialAdapter.uniformModel
            vertexTabText: "Vertex Shader"
            fragmentTabText: "Fragment Shader"
            SplitView.preferredWidth: window.width * 0.5
            SplitView.fillWidth: true
            materialAdapter: materialAdapter
        }
        Preview {
            id: preview
            implicitWidth: parent.width * 0.5
            currentMaterial: materialAdapter.material
        }
    }

    function outputLine(lineText) {
        // Prepend
        editorView.outputTextItem.text = lineText + "\n" + editorView.outputTextItem.text;
    }

    function printShaderStatusError(stage, msg) {
        let outputString = ""
        outputString += msg.filename + " => " + msg.message
        if (msg.identifier !== null && msg.identifier !== "")
            outputString += " '" + msg.identifier + "'";
        if (msg.line >= 0)
            outputString += ", on line: " + msg.line
        outputLine(outputString)
    }

    MaterialAdapter {
        id: materialAdapter
        vertexShader: editorView.vertexEditor.text
        fragmentShader: editorView.fragmentEditor.text
        rootNode: preview.rootNode
        uniformModel: editorView.uniformModel
        onVertexStatusChanged: {
            if (vertexStatus.status !== ShaderConstants.Success) {
                editorView.tabBarInfoView.currentIndex = 1
                printShaderStatusError(ShaderConstants.Vertex, vertexStatus)
            } else if (fragmentStatus.status === ShaderConstants.Success){
                // both work, clear
                editorView.outputTextItem.text = "";
            }
        }
        onFragmentStatusChanged: {
            if (fragmentStatus.status !== ShaderConstants.Success) {
                editorView.tabBarInfoView.currentIndex = 1
                printShaderStatusError(ShaderConstants.Fragment, fragmentStatus)
            } else if (vertexStatus.status === ShaderConstants.Success) {
                // both work, clear
                editorView.outputTextItem.text = "";
            }
        }

        onVertexShaderChanged: {
            editorView.vertexEditor.text = materialAdapter.vertexShader
        }
        onFragmentShaderChanged: {
            editorView.fragmentEditor.text = materialAdapter.fragmentShader
        }
    }
}
