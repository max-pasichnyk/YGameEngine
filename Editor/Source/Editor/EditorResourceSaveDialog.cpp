#include "Editor/PrecompiledHeader.h"
#include "Editor/EditorResourceSaveDialog.h"
#include "Editor/EditorResourcePreviewWidget.h"
#include "Editor/Editor.h"
#include "Editor/EditorHelpers.h"
#include "Engine/ResourceManager.h"
Log_SetChannel(EditorResourceSaveDialog);

EditorResourceSaveDialog::EditorResourceSaveDialog(QWidget *pParent, const ResourceTypeInfo *pResourceTypeInfo)
    : QDialog(pParent, Qt::WindowTitleHint),
      m_pResourceTypeInfo(pResourceTypeInfo),
      m_pModel(NULL)
{
    CreateUI();
    NavigateToDirectory("/", false);
}

EditorResourceSaveDialog::~EditorResourceSaveDialog()
{

}

bool EditorResourceSaveDialog::NavigateToDirectory(const char *directory, bool addToHistory /* = true */)
{
    if (m_pModel != NULL)
    {
        m_pTreeView->setModel(NULL);
        delete m_pModel;
    }

    QString directoryVisual;
    if (directory == NULL || directory[0] == '\0' || (directory[0] == '/' && directory[1] == '\0'))
    {
        m_pModel = new EditorResourceSelectionDialogModel(EmptyString, true, true, 0, this);
        directoryVisual = "/";
    }
    else
    {
        m_pModel = new EditorResourceSelectionDialogModel(directory, true, true, 0, this);
        directoryVisual = directory;
    }

    m_pModel->SetResourceFilter(&m_pResourceTypeInfo, 1);
    m_pTreeView->setModel(m_pModel);

    // update the combo box
    m_pDirectoryComboBox->clear();

    // split it at /
    QString tempPath;
    QStringList directoryComponents = directoryVisual.split('/', QString::SkipEmptyParts);
    for (int i = 0; i < directoryComponents.size(); i++)
    {
        if (tempPath.length() > 0)
            tempPath.append('/');
        tempPath.append(directoryComponents[i]);
        m_pDirectoryComboBox->insertItem(0, g_pEditor->GetIconLibrary()->GetListViewDirectoryIcon(), tempPath);
    }

    // add the root
    m_pDirectoryComboBox->addItem(g_pEditor->GetIconLibrary()->GetListViewDirectoryIcon(), "/");
    m_pDirectoryComboBox->setCurrentIndex(0);

    // add to history
    if (addToHistory)
        m_history.push(ConvertStringToQString(m_pModel->GetRootPath()));

    // update button states
    m_pBackButton->setEnabled((m_history.size() > 0));
    m_pUpDirectoryButton->setEnabled((m_pModel->GetRootPath().GetLength() > 0));

    // clear any preview
    m_pSaveButton->setEnabled(false);
    return true;
}

void EditorResourceSaveDialog::CreateUI()
{
    QToolButton *pMakeDirectoryButton;
    QPushButton *pCancelButton;

    // setup form
    setWindowTitle(QStringLiteral("Select Resource"));
    resize(960, 400);

    // create ui elements
    QGridLayout *gridLayout = new QGridLayout(this);

    QHBoxLayout *hboxLayout = new QHBoxLayout();
    {
        m_pDirectoryComboBox = new QComboBox(this);
        m_pDirectoryComboBox->setEditable(true);
        hboxLayout->addWidget(m_pDirectoryComboBox, 1);

        QHBoxLayout *hboxLayout2 = new QHBoxLayout();
        {
            m_pBackButton = new QToolButton(this);
            m_pBackButton->setEnabled(false);
            m_pBackButton->setIcon(g_pEditor->GetIconLibrary()->GetIconByName("Back", 16));
            hboxLayout2->addWidget(m_pBackButton);

            m_pUpDirectoryButton = new QToolButton(this);
            m_pUpDirectoryButton->setEnabled(false);
            m_pUpDirectoryButton->setIcon(g_pEditor->GetIconLibrary()->GetIconByName("UpDirectory", 16));
            hboxLayout2->addWidget(m_pUpDirectoryButton);

            pMakeDirectoryButton = new QToolButton(this);
            pMakeDirectoryButton->setIcon(g_pEditor->GetIconLibrary()->GetIconByName("NewDirectory", 16));
            hboxLayout2->addWidget(pMakeDirectoryButton);
        }
        hboxLayout->addLayout(hboxLayout2);
    }
    gridLayout->addLayout(hboxLayout, 0, 0, 1, 1);

    //m_pTreeView = new QTreeView(this);
    //m_pTreeView->setExpandsOnDoubleClick(false);
    //gridLayout->addWidget(m_pTreeView, 1, 0, 1, 1);

    m_pTreeView = new QTreeView(this);
    m_pTreeView->setExpandsOnDoubleClick(false);
    gridLayout->addWidget(m_pTreeView, 1, 0, 1, 1);

    m_pResourceNameComboBox = new QComboBox(this);
    m_pResourceNameComboBox->setEditable(true);
    gridLayout->addWidget(m_pResourceNameComboBox, 2, 0, 1, 1);

    hboxLayout = new QHBoxLayout();
    {
        m_pFilterTextLabel = new QLabel(this);
        hboxLayout->addWidget(m_pFilterTextLabel, 1);

        QHBoxLayout *hboxLayout2 = new QHBoxLayout();
        {
            m_pSaveButton = new QPushButton(this);
            m_pSaveButton->setText(tr("Save"));
            m_pSaveButton->setDefault(true);
            hboxLayout2->addWidget(m_pSaveButton);

            pCancelButton = new QPushButton(this);
            pCancelButton->setText(tr("Cancel"));
            hboxLayout2->addWidget(pCancelButton);
        }
        hboxLayout->addLayout(hboxLayout2);
    }
    gridLayout->addLayout(hboxLayout, 3, 0, 1, 1);

    //////////////////////////////////////////////////////////////////////////

    connect(m_pDirectoryComboBox, SIGNAL(activated(const QString &)), this, SLOT(OnDirectoryComboBoxActivated(const QString &)));
    connect(m_pBackButton, SIGNAL(clicked()), this, SLOT(OnBackButtonPressed()));
    connect(m_pUpDirectoryButton, SIGNAL(clicked()), this, SLOT(OnUpDirectoryButtonPressed()));
    connect(m_pTreeView, SIGNAL(activated(const QModelIndex &)), this, SLOT(OnTreeViewItemActivated(const QModelIndex &)));
    connect(m_pTreeView, SIGNAL(clicked(const QModelIndex &)), this, SLOT(OnTreeViewItemClicked(const QModelIndex &)));
    connect(m_pResourceNameComboBox, SIGNAL(editTextChanged(const QString &)), this, SLOT(OnResourceNameEditTextChanged(const QString &)));
    connect(m_pSaveButton, SIGNAL(clicked()), this, SLOT(OnSaveButtonClicked()));
    connect(pCancelButton, SIGNAL(clicked()), this, SLOT(OnCancelButtonClicked()));
}

void EditorResourceSaveDialog::UpdateReturnValue()
{
    QString name = m_pResourceNameComboBox->currentText();
    if (name.length() == 0)
    {
        m_returnValueResourceName = EmptyString;
        m_pSaveButton->setEnabled(false);
        return;
    }

    m_returnValueResourceName.Clear();

    if (m_pModel->GetRootPath().GetLength() > 0)
    {
        m_returnValueResourceName.AppendString(m_pModel->GetRootPath());
        m_returnValueResourceName.AppendCharacter('/');
    }

    m_returnValueResourceName.AppendString(ConvertQStringToString(name));
    m_pSaveButton->setEnabled(true);
}

void EditorResourceSaveDialog::OnDirectoryComboBoxActivated(const QString &text)
{
    // clear any leading /
    String directoryPath(ConvertQStringToString(text));
    if (directoryPath.GetLength() > 0 && directoryPath[0] == '/')
        directoryPath.Erase(0, 1);

    // navigate to this path
    NavigateToDirectory(directoryPath);
}

void EditorResourceSaveDialog::OnBackButtonPressed()
{
    if (m_history.size() > 0)
    {
        String newPath(ConvertQStringToString(m_history.top()));
        m_history.pop();

        NavigateToDirectory(newPath, false);
    }
}

void EditorResourceSaveDialog::OnUpDirectoryButtonPressed()
{
    if (m_pModel->GetRootPath().GetLength() > 0)
    {
        int32 pos = m_pModel->GetRootPath().RFind('/');
        if (pos >= 0)
        {
            PathString newPath;
            newPath.AppendSubString(m_pModel->GetRootPath(), 0, pos);
            NavigateToDirectory(newPath);
        }
        else
        {
            // go to root
            NavigateToDirectory("/");
        }
    }
}

void EditorResourceSaveDialog::OnMakeDirectoryButtonPressed()
{

}

void EditorResourceSaveDialog::OnTreeViewItemActivated(const QModelIndex &index)
{
    const EditorResourceSelectionDialogModel::DirectoryNode *pDirectoryNode = m_pModel->GetDirectoryNodeForIndex(index);
    if (pDirectoryNode == NULL)
        return;

    if (pDirectoryNode->IsDirectory())
    {
        // enter this directory, have to temporarily copy the string though, as navigating will destroy the node
        String directoryPathCopy(pDirectoryNode->GetFullName());
        NavigateToDirectory(directoryPathCopy);
    }
    else
    {
        // select this item
        OnSaveButtonClicked();
    }
}

void EditorResourceSaveDialog::OnTreeViewItemClicked(const QModelIndex &index)
{
    const EditorResourceSelectionDialogModel::DirectoryNode *pDirectoryNode = m_pModel->GetDirectoryNodeForIndex(index);
    if (pDirectoryNode == NULL)
        return;

    // resource?
    if (pDirectoryNode->GetResourceTypeInfo() != NULL)
    {
        // pick up the parent folder names
        PathString relativeName;
        relativeName.AppendString(pDirectoryNode->GetDisplayName());

        const EditorResourceSelectionDialogModel::DirectoryNode *pParentNode = pDirectoryNode->GetParent();
        while (pParentNode->GetParent() != NULL)
        {
            relativeName.PrependFormattedString("%s/", pParentNode->GetDisplayName().GetCharArray());
            pParentNode = pParentNode->GetParent();
        }

        m_pResourceNameComboBox->setCurrentText(ConvertStringToQString(relativeName));
    }
}

void EditorResourceSaveDialog::OnResourceNameEditTextChanged(const QString &contents)
{
    UpdateReturnValue();
}

void EditorResourceSaveDialog::OnSaveButtonClicked()
{
    done(1);
}

void EditorResourceSaveDialog::OnCancelButtonClicked()
{
    m_pResourceNameComboBox->setCurrentText(QStringLiteral(""));
    done(0);
}
