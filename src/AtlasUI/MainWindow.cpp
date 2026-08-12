#include "MainWindow.h"
#include "ThumbnailCache.h"

#include <AtlasCore/Asset.h>
#include <AtlasCore/FileOperations.h>
#include <AtlasCore/StorageAnalyzer.h>
#include <AtlasDatabase/Catalog.h>
#include <AtlasScanner/Scanner.h>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QProgressBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <functional>
#include <vector>

namespace atlas {

template <typename View>
class DropView final : public View {
 public:
  std::function<void(const QStringList&, bool)> onFilesDropped;

 protected:
  void dragEnterEvent(QDragEnterEvent* event) override {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else View::dragEnterEvent(event);
  }
  void dragMoveEvent(QDragMoveEvent* event) override {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else View::dragMoveEvent(event);
  }
  void dropEvent(QDropEvent* event) override {
    if (!event->mimeData()->hasUrls()) { View::dropEvent(event); return; }
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
    if (!paths.isEmpty() && onFilesDropped) {
      const bool move = event->dropAction() == Qt::MoveAction || event->modifiers().testFlag(Qt::ShiftModifier);
      onFilesDropped(paths, move);
      event->acceptProposedAction();
    }
  }
};

class ExplorerPane final : public QWidget {
 public:
  explicit ExplorerPane(QFileSystemModel* model, QWidget* parent = nullptr)
      : QWidget(parent), model_(model) {
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);

    auto* iconDropView = new DropView<QListView>;
    icons_ = iconDropView;
    icons_->setModel(proxy_);
    icons_->setViewMode(QListView::IconMode);
    icons_->setResizeMode(QListView::Adjust);
    icons_->setMovement(QListView::Static);
    icons_->setIconSize({96, 96});
    icons_->setGridSize({148, 132});
    icons_->setUniformItemSizes(true);
    icons_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    icons_->setDragEnabled(true);
    icons_->setAcceptDrops(true);
    icons_->setDragDropMode(QAbstractItemView::DragDrop);
    icons_->setDropIndicatorShown(true);

    auto* detailDropView = new DropView<QTreeView>;
    details_ = detailDropView;
    details_->setModel(proxy_);
    details_->setRootIsDecorated(false);
    details_->setItemsExpandable(false);
    details_->setSortingEnabled(true);
    details_->sortByColumn(0, Qt::AscendingOrder);
    details_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    details_->setDragEnabled(true);
    details_->setAcceptDrops(true);
    details_->setDragDropMode(QAbstractItemView::DragDrop);
    details_->setDropIndicatorShown(true);
    details_->setAlternatingRowColors(true);
    details_->header()->setStretchLastSection(false);
    details_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    stack_ = new QStackedWidget;
    stack_->addWidget(icons_);
    stack_->addWidget(details_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);

    const auto activate = [this](const QModelIndex& proxyIndex) {
      const auto source = proxy_->mapToSource(proxyIndex);
      if (model_->isDir(source)) navigate(model_->filePath(source));
      else QDesktopServices::openUrl(QUrl::fromLocalFile(model_->filePath(source)));
    };
    connect(icons_, &QListView::doubleClicked, this, activate);
    connect(details_, &QTreeView::doubleClicked, this, activate);
    const auto selectionChanged = [this] {
      if (onSelectionChanged) onSelectionChanged(selectedPaths());
    };
    connect(icons_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [selectionChanged](const auto&, const auto&) { selectionChanged(); });
    connect(details_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [selectionChanged](const auto&, const auto&) { selectionChanged(); });
    iconDropView->onFilesDropped = [this](const QStringList& paths, bool move) {
      if (onFilesDropped) onFilesDropped(paths, path_, move);
    };
    detailDropView->onFilesDropped = iconDropView->onFilesDropped;
  }

  void navigate(const QString& requested, bool record = true) {
    QFileInfo folder(requested);
    if (!folder.exists() || !folder.isDir()) return;
    auto canonical = folder.canonicalFilePath();
    if (canonical.isEmpty()) canonical = folder.absoluteFilePath();
    if (record) {
      while (history_.size() > historyIndex_ + 1) history_.removeLast();
      if (history_.isEmpty() || history_.last() != canonical) history_.append(canonical);
      historyIndex_ = history_.size() - 1;
    }
    path_ = canonical;
    const auto source = model_->index(canonical);
    const auto root = proxy_->mapFromSource(source);
    icons_->setRootIndex(root);
    details_->setRootIndex(root);
    if (onPathChanged) onPathChanged(path_);
  }

  void back() {
    if (!canBack()) return;
    navigate(history_.at(--historyIndex_), false);
  }
  void forward() {
    if (!canForward()) return;
    navigate(history_.at(++historyIndex_), false);
  }
  void up() { navigate(QFileInfo(path_).dir().absolutePath()); }
  [[nodiscard]] bool canBack() const { return historyIndex_ > 0; }
  [[nodiscard]] bool canForward() const { return historyIndex_ + 1 < history_.size(); }
  [[nodiscard]] QString path() const { return path_; }
  void setFilter(const QString& filter) {
    proxy_->setFilterRegularExpression(QRegularExpression(QRegularExpression::escape(filter),
                                                           QRegularExpression::CaseInsensitiveOption));
  }
  void setDetails(bool details) { stack_->setCurrentWidget(details ? static_cast<QWidget*>(details_) : icons_); }
  [[nodiscard]] bool showingDetails() const { return stack_->currentWidget() == details_; }
  [[nodiscard]] QStringList selectedPaths() const {
    const auto* view = stack_->currentWidget() == details_ ? static_cast<QAbstractItemView*>(details_)
                                                           : static_cast<QAbstractItemView*>(icons_);
    QStringList paths;
    for (const auto& proxyIndex : view->selectionModel()->selectedRows(0)) {
      paths.append(model_->filePath(proxy_->mapToSource(proxyIndex)));
    }
    return paths;
  }

  std::function<void(const QString&)> onPathChanged;
  std::function<void(const QStringList&)> onSelectionChanged;
  std::function<void(const QStringList&, const QString&, bool)> onFilesDropped;

 private:
  QFileSystemModel* model_{};
  QSortFilterProxyModel* proxy_{};
  QListView* icons_{};
  QTreeView* details_{};
  QStackedWidget* stack_{};
  QString path_;
  QStringList history_;
  qsizetype historyIndex_{-1};
};

class BrowserTab final : public QSplitter {
 public:
  explicit BrowserTab(QFileSystemModel* model, const QString& path, QWidget* parent = nullptr)
      : QSplitter(Qt::Horizontal, parent) {
    primary_ = new ExplorerPane(model);
    secondary_ = new ExplorerPane(model);
    addWidget(primary_);
    addWidget(secondary_);
    secondary_->hide();
    setStretchFactor(0, 1);
    setStretchFactor(1, 1);
    active_ = primary_;
    const auto bind = [this](ExplorerPane* pane) {
      pane->onPathChanged = [this, pane](const QString& path) {
        active_ = pane;
        if (onPathChanged) onPathChanged(path);
      };
      pane->onSelectionChanged = [this, pane](const QStringList& paths) {
        active_ = pane;
        if (onSelectionChanged) onSelectionChanged(paths);
      };
      pane->onFilesDropped = [this](const QStringList& paths, const QString& destination, bool move) {
        if (onFilesDropped) onFilesDropped(paths, destination, move);
      };
    };
    bind(primary_);
    bind(secondary_);
    primary_->navigate(path);
    secondary_->navigate(path);
  }

  ExplorerPane* activePane() const { return active_; }
  void setDualPane(bool enabled) {
    secondary_->setVisible(enabled);
    if (!enabled) active_ = primary_;
    if (enabled && secondary_->path().isEmpty()) secondary_->navigate(primary_->path());
  }
  bool dualPane() const { return secondary_->isVisible(); }
  void setDetails(bool value) { primary_->setDetails(value); secondary_->setDetails(value); }
  void setFilter(const QString& value) { active_->setFilter(value); }
  QStringList sessionPaths() const { return {primary_->path(), secondary_->path()}; }

  std::function<void(const QString&)> onPathChanged;
  std::function<void(const QStringList&)> onSelectionChanged;
  std::function<void(const QStringList&, const QString&, bool)> onFilesDropped;

 private:
  ExplorerPane* primary_{};
  ExplorerPane* secondary_{};
  ExplorerPane* active_{};
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), jobs_{} {
  setWindowTitle("Morphis Atlas");
  resize(1500, 920);
  setDockNestingEnabled(true);
  setAcceptDrops(true);

  model_ = new QFileSystemModel(this);
  model_->setReadOnly(true);
  model_->setRootPath({});
  model_->setOption(QFileSystemModel::DontUseCustomDirectoryIcons, true);

  const auto databasePath = catalogPath();
  std::filesystem::create_directories(databasePath.parent_path());
  catalog_ = std::make_unique<Catalog>(databasePath);

  tabs_ = new QTabWidget;
  tabs_->setTabsClosable(true);
  tabs_->setMovable(true);
  tabs_->setDocumentMode(true);
  setCentralWidget(tabs_);

  createActions();
  createMenusAndToolbars();
  createDocks();

  connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
    if (tabs_->count() == 1) addTab(QDir::homePath());
    delete tabs_->widget(index);
    updateNavigationState();
  });
  connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateNavigationState(); });

  jobs_.setObserver([this](const JobSnapshot& snapshot) {
    QMetaObject::invokeMethod(this, [this, snapshot] {
      QTreeWidgetItem* item{};
      for (int row = 0; row < jobsView_->topLevelItemCount(); ++row) {
        if (jobsView_->topLevelItem(row)->data(0, Qt::UserRole).toULongLong() == snapshot.id) {
          item = jobsView_->topLevelItem(row);
          break;
        }
      }
      if (!item) {
        item = new QTreeWidgetItem(jobsView_);
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(snapshot.id));
      }
      item->setText(0, QString::fromStdString(snapshot.name));
      item->setText(1, jobStateName(snapshot.state));
      item->setText(2, QString::number(qRound(snapshot.progress.fraction * 100.0)) + "%");
      item->setToolTip(0, QString::fromStdString(snapshot.error.empty() ? snapshot.progress.detail : snapshot.error));
      qInfo().noquote() << "Job" << snapshot.id << QString::fromStdString(snapshot.name)
                        << jobStateName(snapshot.state) << QString::fromStdString(snapshot.progress.detail);
      if (snapshot.state == JobState::Failed) statusBar()->showMessage("Job failed: " + QString::fromStdString(snapshot.error), 8000);
      if (snapshot.state == JobState::Completed && !QString::fromStdString(snapshot.name).startsWith("Preview ")) model_->setRootPath({});
    }, Qt::QueuedConnection);
  });

  restoreSession();
  refreshBookmarks();
  statusBar()->showMessage("Ready — automatic indexing and previews never modify originals");
}

MainWindow::~MainWindow() = default;

std::filesystem::path MainWindow::catalogPath() const {
  return std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toStdWString()) / L"atlas.db";
}

BrowserTab* MainWindow::currentTab() const {
  return dynamic_cast<BrowserTab*>(tabs_->currentWidget());
}

void MainWindow::createActions() {
  backAction_ = new QAction("Back", this);
  backAction_->setShortcut(QKeySequence::Back);
  connect(backAction_, &QAction::triggered, this, [this] { if (currentTab()) currentTab()->activePane()->back(); });
  forwardAction_ = new QAction("Forward", this);
  forwardAction_->setShortcut(QKeySequence::Forward);
  connect(forwardAction_, &QAction::triggered, this, [this] { if (currentTab()) currentTab()->activePane()->forward(); });
  upAction_ = new QAction("Up", this);
  upAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
  connect(upAction_, &QAction::triggered, this, [this] { if (currentTab()) currentTab()->activePane()->up(); });
  dualPaneAction_ = new QAction("Dual pane", this);
  dualPaneAction_->setCheckable(true);
  connect(dualPaneAction_, &QAction::toggled, this, [this](bool enabled) { if (currentTab()) currentTab()->setDualPane(enabled); });
  detailsAction_ = new QAction("Details view", this);
  detailsAction_->setCheckable(true);
  connect(detailsAction_, &QAction::toggled, this, [this](bool enabled) { if (currentTab()) currentTab()->setDetails(enabled); });
}

void MainWindow::createMenusAndToolbars() {
  auto* file = menuBar()->addMenu("File");
  auto* newTab = file->addAction("New tab");
  newTab->setShortcut(QKeySequence::AddTab);
  connect(newTab, &QAction::triggered, this, [this] { addTab(currentTab() ? currentTab()->activePane()->path() : QDir::homePath()); });
  auto* newFolderAction = file->addAction("New folder");
  newFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
  connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
  file->addSeparator();
  connect(file->addAction("Copy to…"), &QAction::triggered, this, [this] { transferSelection(false); });
  connect(file->addAction("Move to…"), &QAction::triggered, this, [this] { transferSelection(true); });
  connect(file->addAction("Rename…"), &QAction::triggered, this, &MainWindow::renameSelection);
  connect(file->addAction("Duplicate"), &QAction::triggered, this, &MainWindow::duplicateSelection);
  auto* trash = file->addAction("Move to Recycle Bin");
  trash->setShortcut(QKeySequence::Delete);
  connect(trash, &QAction::triggered, this, &MainWindow::trashSelection);
  file->addSeparator();
  connect(file->addAction("Compress to ZIP/7Z…"), &QAction::triggered, this, &MainWindow::archiveSelection);
  connect(file->addAction("Extract archive…"), &QAction::triggered, this, &MainWindow::extractSelection);
  file->addSeparator();
  connect(file->addAction("Properties"), &QAction::triggered, this, &MainWindow::revealProperties);
  connect(file->addAction("Exit"), &QAction::triggered, this, &QWidget::close);

  auto* navigateMenu = menuBar()->addMenu("Navigate");
  navigateMenu->addActions({backAction_, forwardAction_, upAction_});
  connect(navigateMenu->addAction("Add bookmark"), &QAction::triggered, this, &MainWindow::addCurrentBookmark);
  auto* index = navigateMenu->addAction("Index this folder");
  index->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
  connect(index, &QAction::triggered, this, &MainWindow::indexCurrentFolder);

  auto* view = menuBar()->addMenu("View");
  view->addAction(dualPaneAction_);
  view->addAction(detailsAction_);
  connect(view->addAction("Statistics"), &QAction::triggered, this, &MainWindow::showStatistics);

  auto* searchMenu = menuBar()->addMenu("Search");
  auto* globalSearch = searchMenu->addAction("Search indexed assets…");
  globalSearch->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
  connect(globalSearch, &QAction::triggered, this, &MainWindow::showGlobalSearch);

  auto* toolbar = addToolBar("Navigation");
  toolbar->setMovable(false);
  toolbar->addActions({backAction_, forwardAction_, upAction_});
  auto* tabButton = toolbar->addAction("+");
  connect(tabButton, &QAction::triggered, this, [this] { addTab(currentTab() ? currentTab()->activePane()->path() : QDir::homePath()); });
  location_ = new QLineEdit;
  location_->setClearButtonEnabled(true);
  location_->setPlaceholderText("Path");
  toolbar->addWidget(location_);
  search_ = new QLineEdit;
  search_->setMaximumWidth(300);
  search_->setClearButtonEnabled(true);
  search_->setPlaceholderText("Filter this folder");
  toolbar->addWidget(search_);
  connect(location_, &QLineEdit::returnPressed, this, [this] {
    if (currentTab()) currentTab()->activePane()->navigate(location_->text());
  });
  connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
    if (currentTab()) currentTab()->setFilter(text);
  });
}

void MainWindow::createDocks() {
  auto* foldersDock = new QDockWidget("Folders", this);
  folderTree_ = new QTreeView;
  folderTree_->setModel(model_);
  folderTree_->setHeaderHidden(true);
  for (int column = 1; column < model_->columnCount(); ++column) folderTree_->hideColumn(column);
  foldersDock->setWidget(folderTree_);
  addDockWidget(Qt::LeftDockWidgetArea, foldersDock);
  connect(folderTree_, &QTreeView::activated, this, [this](const QModelIndex& index) {
    if (model_->isDir(index) && currentTab()) currentTab()->activePane()->navigate(model_->filePath(index));
  });

  auto* bookmarksDock = new QDockWidget("Bookmarks", this);
  bookmarks_ = new QListWidget;
  bookmarksDock->setWidget(bookmarks_);
  addDockWidget(Qt::LeftDockWidgetArea, bookmarksDock);
  tabifyDockWidget(foldersDock, bookmarksDock);
  foldersDock->raise();
  connect(bookmarks_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
    if (currentTab()) currentTab()->activePane()->navigate(item->data(Qt::UserRole).toString());
  });
  bookmarks_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(bookmarks_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
    if (auto* item = bookmarks_->itemAt(point)) {
      QMenu menu;
      if (menu.addAction("Remove bookmark") == menu.exec(bookmarks_->mapToGlobal(point))) {
        catalog_->removeBookmark(item->data(Qt::UserRole).toString().toStdWString());
        refreshBookmarks();
      }
    }
  });

  auto* previewDock = new QDockWidget("Preview", this);
  auto* previewContainer = new QWidget;
  auto* previewLayout = new QVBoxLayout(previewContainer);
  preview_ = new QLabel("Select an asset");
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setMinimumWidth(280);
  preview_->setMinimumHeight(220);
  preview_->setScaledContents(false);
  metadata_ = new QLabel;
  metadata_->setWordWrap(true);
  previewLayout->addWidget(preview_, 1);
  previewLayout->addWidget(metadata_);
  previewDock->setWidget(previewContainer);
  addDockWidget(Qt::RightDockWidgetArea, previewDock);

  auto* jobsDock = new QDockWidget("Background jobs", this);
  auto* jobsContainer = new QWidget;
  auto* jobsLayout = new QVBoxLayout(jobsContainer);
  jobsLayout->setContentsMargins(0, 0, 0, 0);
  jobsView_ = new QTreeWidget;
  jobsView_->setHeaderLabels({"Job", "State", "Progress"});
  jobsView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  auto* controls = new QHBoxLayout;
  auto* pause = new QPushButton("Pause queue");
  auto* resume = new QPushButton("Resume queue");
  auto* cancel = new QPushButton("Cancel selected");
  controls->addWidget(pause);
  controls->addWidget(resume);
  controls->addStretch();
  controls->addWidget(cancel);
  jobsLayout->addWidget(jobsView_);
  jobsLayout->addLayout(controls);
  connect(pause, &QPushButton::clicked, this, [this] { jobs_.pause(); });
  connect(resume, &QPushButton::clicked, this, [this] { jobs_.resume(); });
  connect(cancel, &QPushButton::clicked, this, [this] {
    if (auto* item = jobsView_->currentItem()) jobs_.cancel(item->data(0, Qt::UserRole).toULongLong());
  });
  jobsDock->setWidget(jobsContainer);
  addDockWidget(Qt::BottomDockWidgetArea, jobsDock);
}

void MainWindow::addTab(const QString& path) {
  auto* tab = new BrowserTab(model_, path);
  tab->onPathChanged = [this, tab](const QString& newPath) {
    const int index = tabs_->indexOf(tab);
    if (index >= 0) tabs_->setTabText(index, QFileInfo(newPath).fileName().isEmpty() ? newPath : QFileInfo(newPath).fileName());
    if (tabs_->currentWidget() == tab) updateNavigationState();
  };
  tab->onSelectionChanged = [this](const QStringList& paths) {
    updatePreview(paths.isEmpty() ? QString{} : paths.first());
    statusBar()->showMessage(paths.isEmpty() ? "Ready" : QString::number(paths.size()) + " selected");
  };
  tab->onFilesDropped = [this](const QStringList& paths, const QString& destination, bool move) {
    std::vector<std::filesystem::path> sources;
    for (const auto& path : paths) {
      if (move && QDir::cleanPath(QFileInfo(path).absolutePath()) == QDir::cleanPath(destination)) continue;
      sources.emplace_back(path.toStdWString());
    }
    if (sources.empty()) return;
    jobs_.submit((move ? "Move " : "Copy ") + std::to_string(sources.size()) + " dropped item(s)",
        [sources = std::move(sources), target = std::filesystem::path(destination.toStdWString()), move](std::stop_token token, const JobReporter& report) {
          const auto result = FileOperations::transfer(move ? FileOperationKind::Move : FileOperationKind::Copy,
              sources, target, ConflictPolicy::Rename, token,
              [&report](const FileOperationProgress& progress) {
                report(progress.total == 0 ? 1.0 : static_cast<double>(progress.completed) / progress.total,
                       progress.source.string());
              });
          if (!result.failed.empty()) throw std::runtime_error(std::to_string(result.failed.size()) + " dropped item(s) failed");
        });
  };
  const int index = tabs_->addTab(tab, QFileInfo(path).fileName());
  tabs_->setCurrentIndex(index);
  updateNavigationState();
}

void MainWindow::restoreSession() {
  QSettings settings;
  restoreGeometry(settings.value("window/geometry").toByteArray());
  restoreState(settings.value("window/state").toByteArray());
  const auto paths = settings.value("session/tabs").toStringList();
  for (const auto& path : paths) if (QFileInfo(path).isDir()) addTab(path);
  if (tabs_->count() == 0) addTab(QDir::homePath());
  detailsAction_->setChecked(settings.value("view/details", false).toBool());
}

void MainWindow::saveSession() {
  QSettings settings;
  settings.setValue("window/geometry", saveGeometry());
  settings.setValue("window/state", saveState());
  QStringList paths;
  for (int index = 0; index < tabs_->count(); ++index) {
    if (const auto* tab = dynamic_cast<BrowserTab*>(tabs_->widget(index))) paths.append(tab->activePane()->path());
  }
  settings.setValue("session/tabs", paths);
  settings.setValue("view/details", detailsAction_->isChecked());
}

void MainWindow::closeEvent(QCloseEvent* event) {
  saveSession();
  QMainWindow::closeEvent(event);
}

void MainWindow::updateNavigationState() {
  auto* tab = currentTab();
  if (!tab) return;
  auto* pane = tab->activePane();
  location_->setText(QDir::toNativeSeparators(pane->path()));
  backAction_->setEnabled(pane->canBack());
  forwardAction_->setEnabled(pane->canForward());
  dualPaneAction_->blockSignals(true);
  dualPaneAction_->setChecked(tab->dualPane());
  dualPaneAction_->blockSignals(false);
  detailsAction_->blockSignals(true);
  detailsAction_->setChecked(pane->showingDetails());
  detailsAction_->blockSignals(false);
}

void MainWindow::updatePreview(const QString& path) {
  if (previewJob_ != 0) jobs_.cancel(previewJob_);
  previewPath_ = path;
  if (path.isEmpty()) {
    preview_->setText("Select an asset");
    preview_->setPixmap({});
    metadata_->clear();
    return;
  }
  const QFileInfo info(path);
  const QMimeDatabase mimeDatabase;
  const auto mime = mimeDatabase.mimeTypeForFile(info);
  metadata_->setText(QString("%1\n%2\n%3\nModified %4")
      .arg(info.fileName(), mime.name(), QLocale{}.formattedDataSize(info.size()),
           QLocale{}.toString(info.lastModified(), QLocale::ShortFormat)));
  preview_->setPixmap({});
  if (mime.name().startsWith("image/")) {
    const auto cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbnails";
    preview_->setText("Loading preview…");
    previewJob_ = jobs_.submit("Preview " + info.fileName().toStdString(), [this, path, cacheRoot](std::stop_token token, const JobReporter&) {
      if (token.stop_requested()) return;
      ThumbnailCache cache(cacheRoot);
      auto image = cache.load(path, {1600, 1200});
      if (token.stop_requested()) return;
      QMetaObject::invokeMethod(this, [this, path, image = std::move(image)] {
        if (previewPath_ != path) return;
        if (image.isNull()) { preview_->setText("Preview unavailable"); return; }
        preview_->setText({});
        preview_->setPixmap(QPixmap::fromImage(image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
      }, Qt::QueuedConnection);
    });
    return;
  }
  preview_->setText(info.isDir() ? "Folder" : QString::fromUtf8(assetKindName(classifyAsset(path.toStdWString())).data()));
}

void MainWindow::refreshBookmarks() {
  bookmarks_->clear();
  const QList<QPair<QString, QString>> defaults{{"Home", QDir::homePath()}, {"Desktop", QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)}, {"Documents", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)}, {"Pictures", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)}};
  for (const auto& [name, path] : defaults) {
    if (path.isEmpty()) continue;
    auto* item = new QListWidgetItem(name, bookmarks_);
    item->setData(Qt::UserRole, path);
    item->setToolTip(path);
  }
  for (const auto& path : catalog_->bookmarks()) {
    const auto value = QString::fromStdWString(path.wstring());
    auto* item = new QListWidgetItem(QFileInfo(value).fileName(), bookmarks_);
    item->setData(Qt::UserRole, value);
    item->setToolTip(value);
  }
}

void MainWindow::addCurrentBookmark() {
  if (!currentTab()) return;
  catalog_->addBookmark(currentTab()->activePane()->path().toStdWString());
  refreshBookmarks();
}

void MainWindow::indexCurrentFolder() {
  if (!currentTab()) return;
  const auto root = currentTab()->activePane()->path().toStdWString();
  const auto database = catalogPath();
  jobs_.submit("Index " + std::filesystem::path(root).filename().string(), [root, database](std::stop_token token, const JobReporter& report) {
    Catalog catalog(database);
    Scanner scanner;
    std::vector<AssetMetadata> batch;
    std::vector<std::filesystem::path> observed;
    batch.reserve(512);
    const auto summary = scanner.scan(root, token, [&](const AssetMetadata& asset) {
      batch.push_back(asset);
      observed.push_back(asset.path);
      if (batch.size() == 512) {
        catalog.upsertBatch(batch);
        batch.clear();
      }
    }, {}, [&](const ScanSummary& summary) {
      report(0.0, std::to_string(summary.files) + " files, " + std::to_string(summary.directories) + " folders");
    });
    report(summary.cancelled ? 0.0 : 1.0, std::to_string(summary.files) + " files indexed");
    if (!batch.empty()) catalog.upsertBatch(batch);
    if (!token.stop_requested()) catalog.removeMissingUnder(root, observed);
  });
}

void MainWindow::showStatistics() {
  if (!currentTab()) return;
  const auto root = std::filesystem::path(currentTab()->activePane()->path().toStdWString());
  jobs_.submit("Analyze " + root.filename().string(), [this, root](std::stop_token token, const JobReporter& report) {
    StorageAnalyzer analyzer;
    auto analysis = analyzer.analyze(root, token, 15, [&report](std::uint64_t files, const auto& path) {
      report(0.0, std::to_string(files) + " files — " + path.filename().string());
    });
    if (analysis.cancelled) return;
    QMetaObject::invokeMethod(this, [this, root, analysis = std::move(analysis)] {
      QString details = "Largest folders\n";
      for (const auto& item : analysis.largestFolders) details += QString::fromStdWString(item.path.wstring()) + " — " + QLocale{}.formattedDataSize(static_cast<qint64>(item.bytes)) + '\n';
      details += "\nLargest files\n";
      for (const auto& item : analysis.largestFiles) details += QString::fromStdWString(item.path.wstring()) + " — " + QLocale{}.formattedDataSize(static_cast<qint64>(item.bytes)) + '\n';
      details += "\nExact duplicate groups\n";
      for (const auto& group : analysis.duplicates) {
        details += QLocale{}.formattedDataSize(static_cast<qint64>(group.bytesPerFile)) + " each:\n";
        for (const auto& path : group.paths) details += "  " + QString::fromStdWString(path.wstring()) + '\n';
      }
      details += "\nEmpty folders\n";
      for (const auto& path : analysis.emptyFolders) details += QString::fromStdWString(path.wstring()) + '\n';
      QMessageBox box(this);
      box.setWindowTitle("Storage analysis");
      box.setIcon(QMessageBox::Information);
      box.setText(QString("%1\n\nFiles: %2\nFolders: %3\nContent size: %4\nDrive used: %5 of %6\nDuplicate groups: %7\nEmpty folders: %8")
          .arg(QString::fromStdWString(root.wstring())).arg(analysis.files).arg(analysis.directories)
          .arg(QLocale{}.formattedDataSize(static_cast<qint64>(analysis.bytes)))
          .arg(QLocale{}.formattedDataSize(static_cast<qint64>(analysis.capacity - analysis.available)))
          .arg(QLocale{}.formattedDataSize(static_cast<qint64>(analysis.capacity)))
          .arg(analysis.duplicates.size()).arg(analysis.emptyFolders.size()));
      box.setDetailedText(details);
      box.exec();
    }, Qt::QueuedConnection);
  });
}

void MainWindow::showGlobalSearch() {
  bool accepted{};
  const auto text = QInputDialog::getText(this, "Search indexed assets",
      "Name/path and optional filters: ext:png min:10MB max:2GB after:2026-01-01",
      QLineEdit::Normal, {}, &accepted);
  if (!accepted || text.trimmed().isEmpty()) return;
  AssetQuery query;
  QStringList words;
  const auto parseBytes = [](QString value) -> std::optional<std::uintmax_t> {
    value = value.trimmed().toUpper();
    std::uintmax_t multiplier = 1;
    if (value.endsWith("KB")) { multiplier = 1024; value.chop(2); }
    else if (value.endsWith("MB")) { multiplier = 1024 * 1024; value.chop(2); }
    else if (value.endsWith("GB")) { multiplier = 1024ull * 1024 * 1024; value.chop(2); }
    else if (value.endsWith("TB")) { multiplier = 1024ull * 1024 * 1024 * 1024; value.chop(2); }
    bool ok{};
    const auto number = value.toDouble(&ok);
    if (!ok || number < 0) return std::nullopt;
    return static_cast<std::uintmax_t>(number * multiplier);
  };
  for (const auto& token : text.split(' ', Qt::SkipEmptyParts)) {
    if (token.startsWith("ext:", Qt::CaseInsensitive)) query.extension = token.mid(4).toStdString();
    else if (token.startsWith("min:", Qt::CaseInsensitive)) query.minimumSize = parseBytes(token.mid(4));
    else if (token.startsWith("max:", Qt::CaseInsensitive)) query.maximumSize = parseBytes(token.mid(4));
    else if (token.startsWith("after:", Qt::CaseInsensitive)) {
      const auto date = QDate::fromString(token.mid(6), Qt::ISODate);
      if (date.isValid()) query.modifiedAfter = std::chrono::system_clock::time_point{std::chrono::milliseconds{date.startOfDay().toMSecsSinceEpoch()}};
    } else words.append(token);
  }
  query.text = words.join(' ').toStdString();
  query.limit = 1000;
  const auto results = catalog_->search(query);
  auto* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Indexed search — " + text);
  dialog->resize(1000, 650);
  auto* layout = new QVBoxLayout(dialog);
  auto* summary = new QLabel(QString("%1 result(s); search is limited to 1,000 results").arg(results.size()));
  auto* table = new QTableWidget(static_cast<int>(results.size()), 4);
  table->setHorizontalHeaderLabels({"Name", "Folder", "Type", "Size"});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  for (std::size_t row = 0; row < results.size(); ++row) {
    const auto& asset = results[row];
    const auto path = QString::fromStdWString(asset.metadata.path.wstring());
    auto* name = new QTableWidgetItem(QFileInfo(path).fileName());
    name->setData(Qt::UserRole, path);
    table->setItem(static_cast<int>(row), 0, name);
    table->setItem(static_cast<int>(row), 1, new QTableWidgetItem(QFileInfo(path).absolutePath()));
    table->setItem(static_cast<int>(row), 2, new QTableWidgetItem(QString::fromUtf8(assetKindName(classifyAsset(asset.metadata.path, asset.metadata.isDirectory)).data())));
    table->setItem(static_cast<int>(row), 3, new QTableWidgetItem(asset.metadata.isDirectory ? QString{} : QLocale{}.formattedDataSize(static_cast<qint64>(asset.metadata.sizeBytes))));
  }
  layout->addWidget(summary);
  layout->addWidget(table);
  connect(table, &QTableWidget::cellDoubleClicked, dialog, [this, dialog, table](int row, int) {
    const auto path = table->item(row, 0)->data(Qt::UserRole).toString();
    if (currentTab()) currentTab()->activePane()->navigate(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath());
    dialog->accept();
  });
  dialog->show();
}

void MainWindow::renameSelection() {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  if (paths.size() != 1) { statusBar()->showMessage("Select exactly one item to rename", 4000); return; }
  const QFileInfo info(paths.first());
  bool accepted{};
  const auto name = QInputDialog::getText(this, "Rename", "New name", QLineEdit::Normal, info.fileName(), &accepted);
  if (!accepted || name.isEmpty() || name == info.fileName()) return;
  try { FileOperations::rename(info.absoluteFilePath().toStdWString(), (info.dir().absolutePath() + '/' + name).toStdWString()); }
  catch (const std::exception& error) { QMessageBox::critical(this, "Rename failed", error.what()); }
}

void MainWindow::duplicateSelection() {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  for (const auto& path : paths) {
    jobs_.submit("Duplicate " + QFileInfo(path).fileName().toStdString(), [source = std::filesystem::path(path.toStdWString())](std::stop_token, const JobReporter&) { const auto destination = FileOperations::duplicate(source); (void)destination; });
  }
}

void MainWindow::transferSelection(bool move) {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  if (paths.isEmpty()) return;
  const auto destination = QFileDialog::getExistingDirectory(this, move ? "Move to" : "Copy to");
  if (destination.isEmpty()) return;
  const auto answer = QMessageBox::question(this, "Conflicts", "Rename incoming items when a name already exists?", QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (answer == QMessageBox::Cancel) return;
  const auto policy = answer == QMessageBox::Yes ? ConflictPolicy::Rename : ConflictPolicy::Skip;
  std::vector<std::filesystem::path> sources;
  for (const auto& path : paths) sources.emplace_back(path.toStdWString());
  jobs_.submit((move ? "Move " : "Copy ") + std::to_string(sources.size()) + " item(s)",
      [sources = std::move(sources), target = std::filesystem::path(destination.toStdWString()), move, policy](std::stop_token token, const JobReporter& report) {
        const auto result = FileOperations::transfer(move ? FileOperationKind::Move : FileOperationKind::Copy, sources, target, policy, token,
            [&report](const FileOperationProgress& progress) { report(progress.total == 0 ? 1.0 : static_cast<double>(progress.completed) / progress.total, progress.source.string()); });
        if (!result.failed.empty()) throw std::runtime_error(std::to_string(result.failed.size()) + " item(s) failed");
      });
}

void MainWindow::trashSelection() {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  if (paths.isEmpty()) return;
  if (QMessageBox::question(this, "Move to Recycle Bin", QString("Move %1 item(s) to the Recycle Bin?").arg(paths.size())) != QMessageBox::Yes) return;
  jobs_.submit("Recycle " + std::to_string(paths.size()) + " item(s)", [paths](std::stop_token token, const JobReporter& report) {
    for (qsizetype index = 0; index < paths.size(); ++index) {
      if (token.stop_requested()) return;
      if (!QFile::moveToTrash(paths.at(index))) throw std::runtime_error("could not recycle " + paths.at(index).toStdString());
      report(static_cast<double>(index + 1) / paths.size(), paths.at(index).toStdString());
    }
  });
}

void MainWindow::createFolder() {
  if (!currentTab()) return;
  bool accepted{};
  const auto name = QInputDialog::getText(this, "New folder", "Folder name", QLineEdit::Normal, "New Folder", &accepted);
  if (accepted && !name.isEmpty() && !QDir(currentTab()->activePane()->path()).mkdir(name)) QMessageBox::critical(this, "New folder", "The folder could not be created.");
}

void MainWindow::revealProperties() {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  if (paths.isEmpty()) return;
  const QFileInfo info(paths.first());
  QMessageBox::information(this, "Properties", QString("%1\n\nType: %2\nSize: %3\nCreated: %4\nModified: %5\nReadable: %6\nWritable: %7")
      .arg(info.absoluteFilePath(), QMimeDatabase{}.mimeTypeForFile(info).comment(), QLocale{}.formattedDataSize(info.size()),
           QLocale{}.toString(info.birthTime(), QLocale::LongFormat), QLocale{}.toString(info.lastModified(), QLocale::LongFormat),
           info.isReadable() ? "Yes" : "No", info.isWritable() ? "Yes" : "No"));
}

void MainWindow::archiveSelection() {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  if (paths.isEmpty()) return;
  const auto output = QFileDialog::getSaveFileName(this, "Create archive", currentTab()->activePane()->path() + "/Archive.zip", "Archives (*.zip *.7z)");
  if (output.isEmpty()) return;
  QStringList arguments{"a", "-y", output};
  arguments.append(paths);
  runArchiveCommand(arguments, "Create archive");
}

void MainWindow::extractSelection() {
  if (!currentTab()) return;
  const auto paths = currentTab()->activePane()->selectedPaths();
  if (paths.size() != 1) { statusBar()->showMessage("Select one archive to extract", 4000); return; }
  const auto destination = QFileDialog::getExistingDirectory(this, "Extract to", QFileInfo(paths.first()).absolutePath());
  if (destination.isEmpty()) return;
  runArchiveCommand({"x", "-y", "-o" + destination, paths.first()}, "Extract archive");
}

void MainWindow::runArchiveCommand(const QStringList& arguments, const QString& description) {
  auto executable = QStandardPaths::findExecutable("7z");
  if (executable.isEmpty()) executable = QStandardPaths::findExecutable("7za");
  if (executable.isEmpty()) {
    QMessageBox::information(this, description, "7-Zip was not found on PATH. Install 7-Zip or add 7z.exe to PATH to enable ZIP, 7Z, and RAR operations.");
    return;
  }
  jobs_.submit(description.toStdString(), [executable, arguments](std::stop_token token, const JobReporter& report) {
    QProcess process;
    process.start(executable, arguments);
    if (!process.waitForStarted()) throw std::runtime_error("could not start 7-Zip");
    while (!process.waitForFinished(200)) {
      if (token.stop_requested()) { process.kill(); process.waitForFinished(); return; }
      report(0.0, "7-Zip is working");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
      throw std::runtime_error(process.readAllStandardError().toStdString());
    }
  });
}

}  // namespace atlas
