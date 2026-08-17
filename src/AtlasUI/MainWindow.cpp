#include "MainWindow.h"
#include "ThumbnailCache.h"

#include <AtlasCore/Asset.h>
#include <AtlasCore/FileOperations.h>
#include <AtlasCore/PbrMaterial.h>
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
#include <QDrag>
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
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QPushButton>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QRegularExpression>
#include <QProgressBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QThreadPool>
#include <QTime>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <vector>

namespace atlas {

namespace {

QString scanSessionStateName(ScanSessionState state) {
  switch (state) {
    case ScanSessionState::Pending: return "Ready";
    case ScanSessionState::Running: return "Scanning";
    case ScanSessionState::Paused: return "Stopped - resumable";
    case ScanSessionState::Completed: return "Completed";
    case ScanSessionState::Failed: return "Failed - resumable";
  }
  return "Unknown";
}

}  // namespace

class DraggablePreviewLabel final : public QLabel {
 public:
  using QLabel::QLabel;

  void setSourcePaths(QStringList paths) {
    sourcePaths_ = std::move(paths);
    setCursor(sourcePaths_.isEmpty() ? Qt::ArrowCursor : Qt::OpenHandCursor);
    setToolTip(sourcePaths_.isEmpty()
                   ? QString{}
                   : QString("Drag %1 selected asset(s) onto a bookmark to copy or move")
                         .arg(sourcePaths_.size()));
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) pressPosition_ = event->position().toPoint();
    QLabel::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (sourcePaths_.isEmpty() || !(event->buttons() & Qt::LeftButton) ||
        (event->position().toPoint() - pressPosition_).manhattanLength() <
            QApplication::startDragDistance()) {
      QLabel::mouseMoveEvent(event);
      return;
    }
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    urls.reserve(sourcePaths_.size());
    for (const auto& path : sourcePaths_) urls.append(QUrl::fromLocalFile(path));
    mime->setUrls(urls);
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    if (const auto pixmap = this->pixmap(Qt::ReturnByValue); !pixmap.isNull()) {
      drag->setPixmap(pixmap.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      drag->setHotSpot({64, 64});
    }
    setCursor(Qt::ClosedHandCursor);
    drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
    setCursor(Qt::OpenHandCursor);
  }

 private:
  QStringList sourcePaths_;
  QPoint pressPosition_;
};

class BookmarkDropList final : public QListWidget {
 public:
  using QListWidget::QListWidget;
  std::function<void(const QStringList&, const QString&)> onFilesDropped;

 protected:
  void dragEnterEvent(QDragEnterEvent* event) override {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    else QListWidget::dragEnterEvent(event);
  }

  void dragMoveEvent(QDragMoveEvent* event) override {
    auto* target = itemAt(event->position().toPoint());
    if (event->mimeData()->hasUrls() && target) {
      setCurrentItem(target);
      event->acceptProposedAction();
    } else {
      event->ignore();
    }
  }

  void dropEvent(QDropEvent* event) override {
    auto* target = itemAt(event->position().toPoint());
    if (!target || !event->mimeData()->hasUrls()) {
      event->ignore();
      return;
    }
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) {
      if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty() && onFilesDropped) {
      onFilesDropped(paths, target->data(Qt::UserRole).toString());
      event->acceptProposedAction();
    }
  }
};

class AssetProxyModel final : public QSortFilterProxyModel {
 public:
  explicit AssetProxyModel(const QString& cacheRoot, QObject* parent = nullptr)
      : QSortFilterProxyModel(parent), cacheRoot_(cacheRoot + "/grid-thumbnails") {}

  void setCacheRoot(const QString& cacheRoot) {
    beginResetModel();
    cacheRoot_ = cacheRoot + "/grid-thumbnails";
    thumbnails_.clear();
    pending_.clear();
    ++generation_;
    endResetModel();
  }

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
    if (role != Qt::DecorationRole || index.column() != 0) return QSortFilterProxyModel::data(index, role);
    const auto* filesystem = qobject_cast<const QFileSystemModel*>(sourceModel());
    if (!filesystem) return QSortFilterProxyModel::data(index, role);
    const auto source = mapToSource(index);
    if (filesystem->isDir(source)) return QSortFilterProxyModel::data(index, role);
    const auto path = filesystem->filePath(source);
    if (classifyAsset(std::filesystem::path(path.toStdWString())) != AssetKind::Image) {
      return QSortFilterProxyModel::data(index, role);
    }
    if (const auto found = thumbnails_.constFind(path); found != thumbnails_.cend()) return found.value();
    if (!pending_.contains(path)) {
      pending_.insert(path);
      QPointer<AssetProxyModel> proxy(const_cast<AssetProxyModel*>(this));
      const auto cacheRoot = cacheRoot_;
      const auto generation = generation_;
      QThreadPool::globalInstance()->start([proxy, path, cacheRoot, generation] {
        ThumbnailCache cache(cacheRoot);
        auto image = cache.load(path, {256, 256});
        if (!proxy) return;
        QMetaObject::invokeMethod(proxy.data(), [proxy, path, image = std::move(image), generation] {
          if (!proxy) return;
          if (proxy->generation_ != generation) return;
          proxy->pending_.remove(path);
          if (image.isNull()) return;
          if (proxy->thumbnails_.size() >= 2000) proxy->thumbnails_.erase(proxy->thumbnails_.begin());
          proxy->thumbnails_.insert(path, QIcon(QPixmap::fromImage(image)));
          const auto* model = qobject_cast<const QFileSystemModel*>(proxy->sourceModel());
          if (!model) return;
          const auto proxyIndex = proxy->mapFromSource(model->index(path));
          if (proxyIndex.isValid()) emit proxy->dataChanged(proxyIndex, proxyIndex, {Qt::DecorationRole});
        }, Qt::QueuedConnection);
      });
    }
    return QSortFilterProxyModel::data(index, role);
  }

 private:
  QString cacheRoot_;
  mutable QHash<QString, QIcon> thumbnails_;
  mutable QSet<QString> pending_;
  mutable std::uint64_t generation_{};
};

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
  explicit ExplorerPane(QFileSystemModel* model, const QString& cacheRoot,
                        QWidget* parent = nullptr)
      : QWidget(parent), model_(model) {
    proxy_ = new AssetProxyModel(cacheRoot, this);
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

    const auto bindContextMenu = [this](QAbstractItemView* view) {
      view->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint& point) {
        const auto index = view->indexAt(point);
        if (index.isValid() && !view->selectionModel()->isSelected(index)) {
          view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
          view->setCurrentIndex(index);
        }
        if (onContextMenu) onContextMenu(selectedPaths(), path_, view->viewport()->mapToGlobal(point));
      });
    };
    bindContextMenu(icons_);
    bindContextMenu(details_);

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
  void setThumbnailSize(int size) {
    icons_->setIconSize({size, size});
    icons_->setGridSize({size + 52, size + 42});
  }
  void setCacheRoot(const QString& path) { proxy_->setCacheRoot(path); }
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
  std::function<void(const QStringList&, const QString&, const QPoint&)> onContextMenu;

 private:
  QFileSystemModel* model_{};
  AssetProxyModel* proxy_{};
  QListView* icons_{};
  QTreeView* details_{};
  QStackedWidget* stack_{};
  QString path_;
  QStringList history_;
  qsizetype historyIndex_{-1};
};

class BrowserTab final : public QSplitter {
 public:
  explicit BrowserTab(QFileSystemModel* model, const QString& path, const QString& cacheRoot,
                      QWidget* parent = nullptr)
      : QSplitter(Qt::Horizontal, parent) {
    primary_ = new ExplorerPane(model, cacheRoot);
    secondary_ = new ExplorerPane(model, cacheRoot);
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
      pane->onContextMenu = [this, pane](const QStringList& paths, const QString& folder, const QPoint& position) {
        active_ = pane;
        if (onContextMenu) onContextMenu(paths, folder, position);
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
  void setThumbnailSize(int value) { primary_->setThumbnailSize(value); secondary_->setThumbnailSize(value); }
  void setCacheRoot(const QString& value) { primary_->setCacheRoot(value); secondary_->setCacheRoot(value); }
  void setFilter(const QString& value) { active_->setFilter(value); }
  QStringList sessionPaths() const { return {primary_->path(), secondary_->path()}; }
  void restoreSplit(const QString& primaryPath, const QString& secondaryPath, bool dual) {
    primary_->navigate(primaryPath);
    secondary_->navigate(secondaryPath.isEmpty() ? primaryPath : secondaryPath);
    setDualPane(dual);
  }

  std::function<void(const QString&)> onPathChanged;
  std::function<void(const QStringList&)> onSelectionChanged;
  std::function<void(const QStringList&, const QString&, bool)> onFilesDropped;
  std::function<void(const QStringList&, const QString&, const QPoint&)> onContextMenu;

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

  QSettings storageSettings;
  cacheRoot_ = QDir::cleanPath(
      storageSettings.value("storage/cacheRoot", systemCacheFolder()).toString());
  if (cacheRoot_.isEmpty() || !QDir{}.mkpath(cacheRoot_)) {
    cacheRoot_ = systemCacheFolder();
    QDir{}.mkpath(cacheRoot_);
  }

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
        jobsView_->setCurrentItem(item);
      }
      item->setText(0, QString::fromStdString(snapshot.name));
      item->setText(1, jobStateName(snapshot.state));
      const auto detail = QString::fromStdString(snapshot.error.empty()
                                                     ? snapshot.progress.detail
                                                     : snapshot.error);
      item->setText(2, detail);
      item->setText(3, snapshot.state == JobState::Running && snapshot.progress.fraction == 0.0
                           ? "Scanning…"
                           : QString::number(qRound(snapshot.progress.fraction * 100.0)) + "%");
      item->setToolTip(2, detail);
      if (snapshot.state == JobState::Running) {
        activityTitle_->setText(QString::fromStdString(snapshot.name));
        if (snapshot.progress.fraction > 0.0) {
          activityProgress_->setRange(0, 1000);
          activityProgress_->setValue(qRound(snapshot.progress.fraction * 1000.0));
          activityProgress_->setFormat("%p%");
        } else {
          activityProgress_->setRange(0, 0);
          activityProgress_->setFormat("Scanning");
        }
        if (!detail.isEmpty() && lastJobDetails_.value(snapshot.id) != detail) {
          lastJobDetails_.insert(snapshot.id, detail);
          scannerActivity_->appendPlainText(
              QTime::currentTime().toString("HH:mm:ss") + "  " + detail);
        }
      } else if (snapshot.state == JobState::Completed || snapshot.state == JobState::Cancelled ||
                 snapshot.state == JobState::Failed) {
        if (activityTitle_->text() == QString::fromStdString(snapshot.name)) {
          activityProgress_->setRange(0, 100);
          activityProgress_->setValue(snapshot.state == JobState::Completed ? 100 : 0);
          activityProgress_->setFormat(jobStateName(snapshot.state));
        }
      }
      qInfo().noquote() << "Job" << snapshot.id << QString::fromStdString(snapshot.name)
                        << jobStateName(snapshot.state) << QString::fromStdString(snapshot.progress.detail);
      if (snapshot.state == JobState::Failed) statusBar()->showMessage("Job failed: " + QString::fromStdString(snapshot.error), 8000);
      if (snapshot.state == JobState::Completed && !QString::fromStdString(snapshot.name).startsWith("Preview ")) {
        model_->setRootPath({});
        statusBar()->showMessage("Completed: " + QString::fromStdString(snapshot.name), 5000);
      }
    }, Qt::QueuedConnection);
  });

  restoreSession();
  refreshBookmarks();
  for (const auto& session : catalog_->scanSessions()) {
    if (session.state == ScanSessionState::Running) {
      catalog_->setScanSessionState(session.id, ScanSessionState::Paused,
                                    "Atlas closed before this scan finished");
    }
  }
  refreshScanSessions();
  statusBar()->showMessage("Start here: click a folder on the left, then use ★ Bookmark or Scan drive");
}

MainWindow::~MainWindow() = default;

std::filesystem::path MainWindow::catalogPath() const {
  return std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toStdWString()) / L"atlas.db";
}

QString MainWindow::systemCacheFolder() const {
  return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
}

void MainWindow::chooseCacheFolder() {
  const auto selected = QFileDialog::getExistingDirectory(
      this, "Choose Atlas cache folder", cacheRoot_, QFileDialog::ShowDirsOnly);
  if (selected.isEmpty()) return;

  const auto candidate = QDir(selected).absolutePath();
  QTemporaryFile writeProbe(QDir(candidate).filePath(".atlas-write-test-XXXXXX"));
  if (!writeProbe.open()) {
    QMessageBox::critical(this, "Cache folder unavailable",
                          "Atlas cannot write to this folder:\n\n" +
                              QDir::toNativeSeparators(candidate));
    return;
  }
  writeProbe.close();
  writeProbe.remove();

  applyCacheFolder(candidate);
  QMessageBox::information(
      this, "Cache folder saved",
      "Atlas will use this folder by default from now on:\n\n" +
          QDir::toNativeSeparators(cacheRoot_) +
          "\n\nGenerated grid thumbnails and previews are stored in separate subfolders. "
          "Existing cache files in the previous location were left untouched.");
}

void MainWindow::resetCacheFolder() {
  applyCacheFolder(systemCacheFolder());
  QMessageBox::information(this, "Cache folder reset",
                           "Atlas now uses the system default cache folder:\n\n" +
                               QDir::toNativeSeparators(cacheRoot_));
}

void MainWindow::applyCacheFolder(const QString& path) {
  const auto normalized = QDir::cleanPath(QDir(path).absolutePath());
  QDir directory;
  if (normalized.isEmpty() || !directory.mkpath(normalized) ||
      !directory.mkpath(normalized + "/grid-thumbnails") ||
      !directory.mkpath(normalized + "/previews")) {
    QMessageBox::critical(this, "Cache folder unavailable",
                          "Atlas could not create its cache folders under:\n\n" +
                              QDir::toNativeSeparators(normalized));
    return;
  }

  cacheRoot_ = normalized;
  QSettings settings;
  settings.setValue("storage/cacheRoot", cacheRoot_);
  settings.sync();
  for (int index = 0; index < tabs_->count(); ++index) {
    if (auto* tab = dynamic_cast<BrowserTab*>(tabs_->widget(index))) {
      tab->setCacheRoot(cacheRoot_);
    }
  }
  statusBar()->showMessage("Default cache folder: " + QDir::toNativeSeparators(cacheRoot_),
                           7000);
  qInfo().noquote() << "Cache root" << cacheRoot_;
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
  dualPaneAction_ = new QAction("Split current tab", this);
  dualPaneAction_->setCheckable(true);
  dualPaneAction_->setToolTip("Give this tab its own independent left/right folders");
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
  auto* copy = file->addAction("Copy");
  copy->setShortcut(QKeySequence::Copy);
  connect(copy, &QAction::triggered, this, [this] { rememberSelection(false); });
  auto* cut = file->addAction("Cut");
  cut->setShortcut(QKeySequence::Cut);
  connect(cut, &QAction::triggered, this, [this] { rememberSelection(true); });
  pasteAction_ = file->addAction("Paste here");
  pasteAction_->setShortcut(QKeySequence::Paste);
  pasteAction_->setEnabled(false);
  connect(pasteAction_, &QAction::triggered, this, &MainWindow::pasteHere);
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
  auto* bookmark = navigateMenu->addAction("Bookmark current folder");
  bookmark->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
  connect(bookmark, &QAction::triggered, this, &MainWindow::addCurrentBookmark);
  auto* index = navigateMenu->addAction("Start saved scan of this folder");
  index->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
  connect(index, &QAction::triggered, this, &MainWindow::indexCurrentFolder);

  auto* view = menuBar()->addMenu("View");
  view->addAction(dualPaneAction_);
  view->addAction(detailsAction_);
  auto* largerThumbnails = view->addAction("Larger thumbnails");
  largerThumbnails->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
  connect(largerThumbnails, &QAction::triggered, this, [this] {
    thumbnailSize_->setValue(thumbnailSize_->value() + 32);
  });
  auto* smallerThumbnails = view->addAction("Smaller thumbnails");
  smallerThumbnails->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
  connect(smallerThumbnails, &QAction::triggered, this, [this] {
    thumbnailSize_->setValue(thumbnailSize_->value() - 32);
  });
  connect(view->addAction("Folder intelligence"), &QAction::triggered, this, &MainWindow::showStatistics);
  view->addSeparator();
  auto* resetLayout = view->addAction("Reset panel layout");
  connect(resetLayout, &QAction::triggered, this, &MainWindow::applyDefaultLayout);

  auto* searchMenu = menuBar()->addMenu("Search");
  auto* globalSearch = searchMenu->addAction("Search indexed assets…");
  globalSearch->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
  connect(globalSearch, &QAction::triggered, this, &MainWindow::showGlobalSearch);

  auto* analyzeMenu = menuBar()->addMenu("Analyze");
  connect(analyzeMenu->addAction("Start saved folder scan…"), &QAction::triggered, this,
          [this] {
    const auto folder = QFileDialog::getExistingDirectory(
        this, "Choose folder to scan", currentTab() ? currentTab()->activePane()->path()
                                                    : QDir::homePath());
    if (!folder.isEmpty()) startPersistentScan(folder, false);
  });
  auto* scanDrive = analyzeMenu->addAction("Start saved scan of current drive…");
  scanDrive->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
  connect(scanDrive, &QAction::triggered, this, &MainWindow::showDriveStatistics);
  analyzeMenu->addSeparator();
  connect(analyzeMenu->addAction("Save selected scan as snapshot…"),
          &QAction::triggered, this, &MainWindow::saveSelectedScanAs);
  connect(analyzeMenu->addAction("Re-organize selected saved scan"),
          &QAction::triggered, this, &MainWindow::showSelectedScanResults);
  analyzeMenu->addSeparator();
  connect(analyzeMenu->addAction("Analyze current folder without saving…"),
          &QAction::triggered, this, &MainWindow::showStatistics);

  auto* settingsMenu = menuBar()->addMenu("Settings");
  auto* chooseCache = settingsMenu->addAction("Choose cache folder...");
  chooseCache->setToolTip("Select the persistent default location for generated thumbnails and previews");
  connect(chooseCache, &QAction::triggered, this, &MainWindow::chooseCacheFolder);
  connect(settingsMenu->addAction("Open cache folder"), &QAction::triggered, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(cacheRoot_));
  });
  settingsMenu->addSeparator();
  connect(settingsMenu->addAction("Reset cache folder to system default"),
          &QAction::triggered, this, &MainWindow::resetCacheFolder);

  auto* help = menuBar()->addMenu("Help");
  connect(help->addAction("Quick start"), &QAction::triggered, this, [this] {
    QMessageBox::information(this, "Atlas quick start",
        "1. Click a folder in the left Folders panel to open it.\n\n"
        "2. Click ★ Bookmark to save the current folder.\n\n"
        "3. Right-click files for Copy, Cut, Paste/Move here, Rename, Duplicate, and Recycle Bin.\n\n"
        "4. Click Drive intelligence to see percentages, the largest-file heatmap, PBR materials, FBX models, archives, and cleanup diagnostics.\n\n"
        "5. Long operations appear in Background jobs and can be cancelled there.");
  });
  connect(help->addAction("About Morphis Atlas"), &QAction::triggered, this, [this] {
    QMessageBox::about(this, "About Morphis Atlas",
        "Morphis Atlas 0.1\n\nA non-destructive digital asset file manager for creative workflows.");
  });

  auto* toolbar = addToolBar("Navigation");
  toolbar->setMovable(false);
  toolbar->addActions({backAction_, forwardAction_, upAction_});
  auto* tabButton = toolbar->addAction("+");
  connect(tabButton, &QAction::triggered, this, [this] { addTab(currentTab() ? currentTab()->activePane()->path() : QDir::homePath()); });
  toolbar->addAction(dualPaneAction_);
  toolbar->addSeparator();
  auto* bookmarkButton = toolbar->addAction("★ Bookmark");
  bookmarkButton->setToolTip("Bookmark the current folder (Ctrl+D)");
  connect(bookmarkButton, &QAction::triggered, this, &MainWindow::addCurrentBookmark);
  auto* scanDriveButton = toolbar->addAction("Scan & organize drive");
  scanDriveButton->setToolTip("Save a resumable scan, prioritizing PBR materials and 3D files in its organized results");
  connect(scanDriveButton, &QAction::triggered, this, &MainWindow::showDriveStatistics);
  toolbar->addSeparator();
  auto* thumbnailLabel = new QLabel("Thumbnail size");
  thumbnailLabel->setToolTip("Resize all grid tiles");
  toolbar->addWidget(thumbnailLabel);
  thumbnailSize_ = new QSlider(Qt::Horizontal);
  thumbnailSize_->setRange(64, 256);
  thumbnailSize_->setSingleStep(16);
  thumbnailSize_->setPageStep(32);
  thumbnailSize_->setTickInterval(32);
  thumbnailSize_->setFixedWidth(150);
  thumbnailSize_->setToolTip("Resize image previews and all other asset icons");
  toolbar->addWidget(thumbnailSize_);
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
  connect(thumbnailSize_, &QSlider::valueChanged, this, [this](int value) {
    for (int index = 0; index < tabs_->count(); ++index) {
      if (auto* tab = dynamic_cast<BrowserTab*>(tabs_->widget(index))) tab->setThumbnailSize(value);
    }
    thumbnailSize_->setToolTip(QString("Thumbnail size: %1 px").arg(value));
  });
}

void MainWindow::createDocks() {
  foldersDock_ = new QDockWidget("Folders", this);
  foldersDock_->setObjectName("FoldersDock");
  folderTree_ = new QTreeView;
  folderTree_->setModel(model_);
  folderTree_->setHeaderHidden(true);
  for (int column = 1; column < model_->columnCount(); ++column) folderTree_->hideColumn(column);
  foldersDock_->setWidget(folderTree_);
  addDockWidget(Qt::LeftDockWidgetArea, foldersDock_);
  connect(folderTree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
    if (model_->isDir(index) && currentTab()) currentTab()->activePane()->navigate(model_->filePath(index));
  });

  bookmarksDock_ = new QDockWidget("Bookmarks", this);
  bookmarksDock_->setObjectName("BookmarksDock");
  auto* bookmarksContainer = new QWidget;
  auto* bookmarksLayout = new QVBoxLayout(bookmarksContainer);
  bookmarksLayout->setContentsMargins(6, 6, 6, 6);
  auto* addBookmark = new QPushButton("★  Bookmark current folder");
  addBookmark->setToolTip("Save the folder shown in the active pane (Ctrl+D)");
  connect(addBookmark, &QPushButton::clicked, this, &MainWindow::addCurrentBookmark);
  auto* bookmarkDropList = new BookmarkDropList;
  bookmarks_ = bookmarkDropList;
  bookmarks_->setAcceptDrops(true);
  bookmarks_->setDragDropMode(QAbstractItemView::DropOnly);
  bookmarks_->setDropIndicatorShown(true);
  auto* hint = new QLabel("Double-click to open · Right-click to remove");
  hint->setObjectName("PanelHint");
  bookmarksLayout->addWidget(addBookmark);
  bookmarksLayout->addWidget(bookmarks_, 1);
  bookmarksLayout->addWidget(hint);
  bookmarksDock_->setWidget(bookmarksContainer);
  addDockWidget(Qt::LeftDockWidgetArea, bookmarksDock_);
  splitDockWidget(foldersDock_, bookmarksDock_, Qt::Vertical);
  connect(bookmarks_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    const auto path = item->data(Qt::UserRole).toString();
    if (!QFileInfo(path).isDir()) {
      QMessageBox::warning(this, "Bookmark unavailable",
                           "This bookmarked folder no longer exists:\n" +
                               QDir::toNativeSeparators(path));
      return;
    }
    if (currentTab()) currentTab()->activePane()->navigate(path);
  });
  bookmarkDropList->onFilesDropped = [this](const QStringList& paths, const QString& destination) {
    QMessageBox choice(this);
    choice.setWindowTitle("Send assets to bookmarked folder");
    choice.setText(QString("Destination\n%1\n\nWhat should Atlas do with %2 selected asset(s)?")
                       .arg(QDir::toNativeSeparators(destination)).arg(paths.size()));
    choice.setInformativeText(
        "Move here removes each original only after its destination copy succeeds. "
        "Copy here keeps the original in place.");
    auto* move = choice.addButton("Move here (remove original)", QMessageBox::DestructiveRole);
    auto* copy = choice.addButton("Copy here (keep original)", QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.setDefaultButton(copy);
    choice.exec();
    if (choice.clickedButton() == move) enqueueTransfer(paths, destination, true, true);
    else if (choice.clickedButton() == copy) enqueueTransfer(paths, destination, false, true);
  };
  bookmarks_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(bookmarks_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
    if (auto* item = bookmarks_->itemAt(point)) {
      QMenu menu;
      auto* open = menu.addAction("Open");
      QAction* remove{};
      if (item->data(Qt::UserRole + 1).toBool()) remove = menu.addAction("Remove bookmark");
      const auto* selected = menu.exec(bookmarks_->mapToGlobal(point));
      if (selected == open) {
        const auto path = item->data(Qt::UserRole).toString();
        if (QFileInfo(path).isDir() && currentTab()) currentTab()->activePane()->navigate(path);
      } else if (remove && selected == remove) {
        catalog_->removeBookmark(item->data(Qt::UserRole).toString().toStdWString());
        refreshBookmarks();
      }
    }
  });

  previewDock_ = new QDockWidget("Preview", this);
  previewDock_->setObjectName("PreviewDock");
  auto* previewContainer = new QWidget;
  auto* previewLayout = new QVBoxLayout(previewContainer);
  preview_ = new DraggablePreviewLabel("Select an asset");
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setMinimumWidth(280);
  preview_->setMinimumHeight(220);
  preview_->setScaledContents(false);
  metadata_ = new QLabel;
  metadata_->setWordWrap(true);
  auto* previewHint = new QLabel("Drag the preview onto a bookmark to copy or move");
  previewHint->setObjectName("PanelHint");
  previewHint->setWordWrap(true);
  previewHint->setAlignment(Qt::AlignCenter);
  previewLayout->addWidget(preview_, 1);
  previewLayout->addWidget(metadata_);
  previewLayout->addWidget(previewHint);
  previewDock_->setWidget(previewContainer);
  addDockWidget(Qt::RightDockWidgetArea, previewDock_);

  jobsDock_ = new QDockWidget("Background jobs", this);
  jobsDock_->setObjectName("JobsDock");
  auto* jobsContainer = new QWidget;
  auto* jobsLayout = new QVBoxLayout(jobsContainer);
  jobsLayout->setContentsMargins(0, 0, 0, 0);
  jobsView_ = new QTreeWidget;
  jobsView_->setHeaderLabels({"Job", "State", "Current activity", "Progress"});
  jobsView_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  jobsView_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  jobsView_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  jobsView_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  activityTitle_ = new QLabel("No active scanner");
  activityTitle_->setStyleSheet("font-weight: 600; padding: 3px;");
  activityProgress_ = new QProgressBar;
  activityProgress_->setRange(0, 100);
  activityProgress_->setValue(0);
  scannerActivity_ = new QPlainTextEdit;
  scannerActivity_->setReadOnly(true);
  scannerActivity_->setMaximumBlockCount(250);
  scannerActivity_->setPlaceholderText(
      "Start a folder or drive scan to see live counts and the current filesystem path.");
  scannerActivity_->setMaximumHeight(110);
  auto* controls = new QHBoxLayout;
  auto* pause = new QPushButton("Pause queue");
  auto* resume = new QPushButton("Resume queue");
  auto* cancel = new QPushButton("Cancel selected");
  auto* clearActivity = new QPushButton("Clear activity");
  controls->addWidget(pause);
  controls->addWidget(resume);
  controls->addStretch();
  controls->addWidget(clearActivity);
  controls->addWidget(cancel);
  jobsLayout->addWidget(activityTitle_);
  jobsLayout->addWidget(activityProgress_);
  jobsLayout->addWidget(jobsView_, 1);
  jobsLayout->addWidget(scannerActivity_);
  jobsLayout->addLayout(controls);
  connect(pause, &QPushButton::clicked, this, [this] { jobs_.pause(); });
  connect(resume, &QPushButton::clicked, this, [this] { jobs_.resume(); });
  connect(cancel, &QPushButton::clicked, this, [this] {
    if (auto* item = jobsView_->currentItem()) jobs_.cancel(item->data(0, Qt::UserRole).toULongLong());
  });
  connect(clearActivity, &QPushButton::clicked, scannerActivity_, &QPlainTextEdit::clear);
  jobsDock_->setWidget(jobsContainer);
  addDockWidget(Qt::BottomDockWidgetArea, jobsDock_);

  scansDock_ = new QDockWidget("Saved scans", this);
  scansDock_->setObjectName("ScansDock");
  auto* scansContainer = new QWidget;
  auto* scansLayout = new QVBoxLayout(scansContainer);
  scansLayout->setContentsMargins(4, 4, 4, 4);
  scansView_ = new QTreeWidget;
  scansView_->setHeaderLabels(
      {"Saved scan", "Source", "State", "Files", "Folders", "Remaining",
       "Indexed content", "Versions", "Updated"});
  scansView_->setSelectionMode(QAbstractItemView::SingleSelection);
  scansView_->setAlternatingRowColors(true);
  scansView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column < 9; ++column) {
    scansView_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  }
  auto* scanControls = new QHBoxLayout;
  auto* scanFolder = new QPushButton("Scan folder…");
  auto* scanDrive = new QPushButton("Scan current drive");
  auto* stopScan = new QPushButton("Stop selected");
  auto* resumeScan = new QPushButton("Resume selected");
  auto* saveScanAs = new QPushButton("Save scan as…");
  auto* viewScan = new QPushButton("View organized results");
  scanControls->addWidget(scanFolder);
  scanControls->addWidget(scanDrive);
  scanControls->addStretch();
  scanControls->addWidget(stopScan);
  scanControls->addWidget(resumeScan);
  scanControls->addWidget(saveScanAs);
  scanControls->addWidget(viewScan);
  scansLayout->addWidget(scansView_, 1);
  scansLayout->addLayout(scanControls);
  connect(scanFolder, &QPushButton::clicked, this, [this] {
    const auto folder = QFileDialog::getExistingDirectory(
        this, "Choose folder to scan", currentTab() ? currentTab()->activePane()->path()
                                                    : QDir::homePath());
    if (!folder.isEmpty()) startPersistentScan(folder, false);
  });
  connect(scanDrive, &QPushButton::clicked, this, &MainWindow::showDriveStatistics);
  connect(stopScan, &QPushButton::clicked, this, &MainWindow::pauseSelectedScan);
  connect(resumeScan, &QPushButton::clicked, this, &MainWindow::resumeSelectedScan);
  connect(saveScanAs, &QPushButton::clicked, this, &MainWindow::saveSelectedScanAs);
  connect(viewScan, &QPushButton::clicked, this, &MainWindow::showSelectedScanResults);
  connect(scansView_, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem*, int) { showSelectedScanResults(); });
  scansDock_->setWidget(scansContainer);
  addDockWidget(Qt::BottomDockWidgetArea, scansDock_);
  tabifyDockWidget(jobsDock_, scansDock_);
  scansDock_->raise();

  auto* panels = menuBar()->addMenu("Panels");
  panels->addAction(foldersDock_->toggleViewAction());
  panels->addAction(bookmarksDock_->toggleViewAction());
  panels->addAction(previewDock_->toggleViewAction());
  panels->addAction(jobsDock_->toggleViewAction());
  panels->addAction(scansDock_->toggleViewAction());
}

void MainWindow::addTab(const QString& path) {
  auto* tab = new BrowserTab(model_, path, cacheRoot_);
  tab->onPathChanged = [this, tab](const QString& newPath) {
    const int index = tabs_->indexOf(tab);
    if (index >= 0) tabs_->setTabText(index, QFileInfo(newPath).fileName().isEmpty() ? newPath : QFileInfo(newPath).fileName());
    if (tabs_->currentWidget() == tab) updateNavigationState();
  };
  tab->onSelectionChanged = [this](const QStringList& paths) {
    updatePreview(paths.isEmpty() ? QString{} : paths.first());
    if (auto* draggable = dynamic_cast<DraggablePreviewLabel*>(preview_)) {
      draggable->setSourcePaths(paths);
    }
    statusBar()->showMessage(paths.isEmpty() ? "Ready" : QString::number(paths.size()) + " selected");
  };
  tab->onFilesDropped = [this](const QStringList& paths, const QString& destination, bool move) {
    QMessageBox choice(this);
    choice.setWindowTitle("Place items here");
    choice.setText(QString("What should Atlas do with %1 item(s)?\n\nDestination: %2")
                       .arg(paths.size()).arg(QDir::toNativeSeparators(destination)));
    auto* copyHere = choice.addButton("Copy here", QMessageBox::AcceptRole);
    auto* moveHere = choice.addButton("Move here", QMessageBox::ActionRole);
    choice.addButton(QMessageBox::Cancel);
    choice.setDefaultButton(move ? moveHere : copyHere);
    choice.exec();
    if (choice.clickedButton() == copyHere) enqueueTransfer(paths, destination, false, true);
    else if (choice.clickedButton() == moveHere) enqueueTransfer(paths, destination, true, true);
  };
  tab->onContextMenu = [this](const QStringList& paths, const QString& folder, const QPoint& position) {
    QMenu menu;
    if (paths.size() == 1 && QFileInfo(paths.first()).isDir()) {
      auto* open = menu.addAction("Open folder");
      connect(open, &QAction::triggered, this, [this, path = paths.first()] {
        if (currentTab()) currentTab()->activePane()->navigate(path);
      });
      auto* bookmark = menu.addAction("★  Bookmark this folder");
      connect(bookmark, &QAction::triggered, this, [this, path = paths.first()] {
        catalog_->addBookmark(path.toStdWString());
        refreshBookmarks();
        statusBar()->showMessage("Folder bookmarked", 3000);
      });
      menu.addSeparator();
    } else {
      auto* bookmark = menu.addAction("★  Bookmark current folder");
      connect(bookmark, &QAction::triggered, this, &MainWindow::addCurrentBookmark);
      menu.addSeparator();
    }
    if (!paths.isEmpty()) {
      auto* copy = menu.addAction("Copy");
      connect(copy, &QAction::triggered, this, [this] { rememberSelection(false); });
      auto* cut = menu.addAction("Cut");
      connect(cut, &QAction::triggered, this, [this] { rememberSelection(true); });
    }
    auto* paste = menu.addAction(transferClipboardMoves_ ? "Move here" : "Paste here");
    paste->setEnabled(!transferClipboard_.isEmpty());
    connect(paste, &QAction::triggered, this, [this, folder] {
      enqueueTransfer(transferClipboard_, folder, transferClipboardMoves_, true);
      if (transferClipboardMoves_) { transferClipboard_.clear(); pasteAction_->setEnabled(false); }
    });
    if (!paths.isEmpty()) {
      menu.addSeparator();
      connect(menu.addAction("Rename…"), &QAction::triggered, this, &MainWindow::renameSelection);
      connect(menu.addAction("Duplicate"), &QAction::triggered, this, &MainWindow::duplicateSelection);
      connect(menu.addAction("Move to Recycle Bin"), &QAction::triggered, this, &MainWindow::trashSelection);
      connect(menu.addAction("Properties"), &QAction::triggered, this, &MainWindow::revealProperties);
    }
    menu.exec(position);
  };
  const int index = tabs_->addTab(tab, QFileInfo(path).fileName());
  if (thumbnailSize_) tab->setThumbnailSize(thumbnailSize_->value());
  tabs_->setCurrentIndex(index);
  updateNavigationState();
}

void MainWindow::restoreSession() {
  QSettings settings;
  restoreGeometry(settings.value("window/geometry").toByteArray());
  if (settings.value("window/layoutVersion", 0).toInt() == 6) {
    restoreState(settings.value("window/state").toByteArray(), 6);
  } else {
    applyDefaultLayout();
  }
  const auto savedTabs = settings.beginReadArray("session/browserTabs");
  for (int index = 0; index < savedTabs; ++index) {
    settings.setArrayIndex(index);
    const auto primary = settings.value("primary").toString();
    const auto secondary = settings.value("secondary").toString();
    const auto dual = settings.value("dual", false).toBool();
    if (!QFileInfo(primary).isDir()) continue;
    addTab(primary);
    currentTab()->restoreSplit(primary, QFileInfo(secondary).isDir() ? secondary : primary, dual);
  }
  settings.endArray();
  if (tabs_->count() == 0) {
    const auto legacyPaths = settings.value("session/tabs").toStringList();
    for (const auto& path : legacyPaths) if (QFileInfo(path).isDir()) addTab(path);
  }
  if (tabs_->count() == 0) addTab(QDir::homePath());
  detailsAction_->setChecked(settings.value("view/details", false).toBool());
  thumbnailSize_->setValue(settings.value("view/thumbnailSize", 128).toInt());
}

void MainWindow::saveSession() {
  QSettings settings;
  settings.setValue("window/geometry", saveGeometry());
  settings.setValue("window/state", saveState());
  settings.setValue("window/layoutVersion", 6);
  QStringList paths;
  settings.beginWriteArray("session/browserTabs", tabs_->count());
  for (int index = 0; index < tabs_->count(); ++index) {
    settings.setArrayIndex(index);
    if (const auto* tab = dynamic_cast<BrowserTab*>(tabs_->widget(index))) {
      const auto splitPaths = tab->sessionPaths();
      settings.setValue("primary", splitPaths.value(0));
      settings.setValue("secondary", splitPaths.value(1));
      settings.setValue("dual", tab->dualPane());
      paths.append(tab->activePane()->path());
    }
  }
  settings.endArray();
  settings.setValue("session/tabs", paths);
  settings.setValue("view/details", detailsAction_->isChecked());
  settings.setValue("view/thumbnailSize", thumbnailSize_->value());
}

void MainWindow::applyDefaultLayout() {
  foldersDock_->show();
  bookmarksDock_->show();
  previewDock_->show();
  jobsDock_->show();
  scansDock_->show();
  addDockWidget(Qt::LeftDockWidgetArea, foldersDock_);
  addDockWidget(Qt::LeftDockWidgetArea, bookmarksDock_);
  splitDockWidget(foldersDock_, bookmarksDock_, Qt::Vertical);
  addDockWidget(Qt::RightDockWidgetArea, previewDock_);
  addDockWidget(Qt::BottomDockWidgetArea, jobsDock_);
  addDockWidget(Qt::BottomDockWidgetArea, scansDock_);
  tabifyDockWidget(jobsDock_, scansDock_);
  resizeDocks({foldersDock_, previewDock_}, {280, 340}, Qt::Horizontal);
  resizeDocks({foldersDock_, bookmarksDock_}, {560, 260}, Qt::Vertical);
  resizeDocks({jobsDock_}, {280}, Qt::Vertical);
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
  const auto folderIndex = model_->index(pane->path());
  if (folderIndex.isValid()) {
    folderTree_->setCurrentIndex(folderIndex);
    folderTree_->scrollTo(folderIndex);
  }
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
  preview_->setText({});
  const auto nativeIcon = model_->fileIcon(model_->index(path));
  const auto iconExtent = std::max(128, std::min(preview_->width() - 24, preview_->height() - 24));
  preview_->setPixmap(nativeIcon.pixmap({iconExtent, iconExtent}));
  if (mime.name().startsWith("image/")) {
    const auto cacheRoot = cacheRoot_ + "/previews";
    previewJob_ = jobs_.submit("Preview " + info.fileName().toStdString(), [this, path, cacheRoot](std::stop_token token, const JobReporter&) {
      if (token.stop_requested()) return;
      ThumbnailCache cache(cacheRoot);
      auto image = cache.load(path, {1600, 1200});
      if (token.stop_requested()) return;
      QMetaObject::invokeMethod(this, [this, path, image = std::move(image)] {
        if (previewPath_ != path) return;
        if (image.isNull()) {
          preview_->setToolTip("The image decoder could not render this file; showing its native icon.");
          return;
        }
        preview_->setText({});
        preview_->setPixmap(QPixmap::fromImage(image).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
      }, Qt::QueuedConnection);
    });
    return;
  }
  preview_->setToolTip(QString("%1\nDrag onto a bookmark to copy or move")
                           .arg(info.isDir() ? "Folder" :
                                QString::fromUtf8(assetKindName(classifyAsset(path.toStdWString())).data())));
}

void MainWindow::refreshBookmarks() {
  bookmarks_->clear();
  const QList<QPair<QString, QString>> defaults{{"Home", QDir::homePath()}, {"Desktop", QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)}, {"Documents", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)}, {"Pictures", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)}};
  for (const auto& [name, path] : defaults) {
    if (path.isEmpty()) continue;
    auto* item = new QListWidgetItem(name, bookmarks_);
    item->setData(Qt::UserRole, path);
    item->setData(Qt::UserRole + 1, false);
    item->setToolTip(path);
    item->setIcon(model_->fileIcon(model_->index(path)));
  }
  const auto savedBookmarks = catalog_->bookmarks();
  for (const auto& path : savedBookmarks) {
    const auto value = QString::fromStdWString(path.wstring());
    auto displayName = QFileInfo(value).fileName();
    if (displayName.isEmpty()) displayName = QDir::toNativeSeparators(value);
    auto* item = new QListWidgetItem("★  " + displayName, bookmarks_);
    item->setData(Qt::UserRole, value);
    item->setData(Qt::UserRole + 1, true);
    item->setToolTip(value);
    item->setIcon(model_->fileIcon(model_->index(value)));
  }
  bookmarksDock_->setWindowTitle(QString("Bookmarks — %1 saved").arg(savedBookmarks.size()));
}

void MainWindow::addCurrentBookmark() {
  if (!currentTab()) return;
  const auto current = QDir::cleanPath(currentTab()->activePane()->path());
  for (int row = 0; row < bookmarks_->count(); ++row) {
    if (QDir::cleanPath(bookmarks_->item(row)->data(Qt::UserRole).toString())
            .compare(current, Qt::CaseInsensitive) == 0) {
      bookmarks_->setCurrentRow(row);
      bookmarksDock_->show();
      bookmarksDock_->raise();
      statusBar()->showMessage("This folder is already bookmarked: " +
                                   QDir::toNativeSeparators(current), 5000);
      return;
    }
  }
  catalog_->addBookmark(current.toStdWString());
  refreshBookmarks();
  for (int row = 0; row < bookmarks_->count(); ++row) {
    if (QDir::cleanPath(bookmarks_->item(row)->data(Qt::UserRole).toString())
            .compare(current, Qt::CaseInsensitive) == 0) {
      bookmarks_->setCurrentRow(row);
      bookmarks_->scrollToItem(bookmarks_->item(row));
      break;
    }
  }
  bookmarksDock_->show();
  bookmarksDock_->raise();
  statusBar()->showMessage("Bookmarked: " + QDir::toNativeSeparators(current), 5000);
}

void MainWindow::indexCurrentFolder() {
  if (!currentTab()) return;
  startPersistentScan(currentTab()->activePane()->path(), false);
}

void MainWindow::startPersistentScan(const QString& path, bool driveScan) {
  const QFileInfo root(path);
  if (!root.exists() || !root.isDir()) {
    QMessageBox::warning(this, "Start scan", "The selected scan location is unavailable.");
    return;
  }
  const auto id = catalog_->createScanSession(root.absoluteFilePath().toStdWString(), driveScan);
  refreshScanSessions();
  scansDock_->show();
  scansDock_->raise();
  for (int row = 0; row < scansView_->topLevelItemCount(); ++row) {
    if (scansView_->topLevelItem(row)->data(0, Qt::UserRole).toLongLong() == id) {
      scansView_->setCurrentItem(scansView_->topLevelItem(row));
      break;
    }
  }
  runPersistentScan(id);
}

void MainWindow::runPersistentScan(std::int64_t sessionId) {
  if (const auto existing = scanJobs_.find(sessionId); existing != scanJobs_.end()) {
    for (const auto& snapshot : jobs_.snapshots()) {
      if (snapshot.id == existing.value() &&
          (snapshot.state == JobState::Queued || snapshot.state == JobState::Running ||
           snapshot.state == JobState::Paused)) return;
    }
  }
  const auto session = catalog_->scanSession(sessionId);
  if (!session || !QFileInfo(QString::fromStdWString(session->root.wstring())).isDir()) {
    if (session) catalog_->setScanSessionState(sessionId, ScanSessionState::Failed,
                                                "Scan root is unavailable");
    refreshScanSessions();
    return;
  }
  const auto database = catalogPath();
  const auto displayName = QString::fromStdWString(session->root.filename().wstring());
  const auto jobId = jobs_.submit(
      "Saved scan " + (displayName.isEmpty() ? session->root.string() : displayName.toStdString()),
      [this, database, sessionId](std::stop_token token, const JobReporter& report) {
    Catalog catalog(database);
    Scanner scanner;
    catalog.setScanSessionState(sessionId, ScanSessionState::Running);
    std::uint64_t completedDirectories{};
    try {
      while (!token.stop_requested()) {
        const auto directory = catalog.nextScanDirectory(sessionId);
        if (!directory) {
          catalog.setScanSessionState(sessionId, ScanSessionState::Completed);
          const auto complete = catalog.scanSession(sessionId);
          report(1.0, complete ? std::to_string(complete->files) +
                                     " files saved and organized"
                               : "Scan completed");
          QMetaObject::invokeMethod(this, [this] { refreshScanSessions(); },
                                    Qt::QueuedConnection);
          return;
        }
        const auto discovered = scanner.scanDirectory(*directory, token);
        if (token.stop_requested()) break;
        catalog.recordScannedDirectory(sessionId, *directory, discovered.entries,
                                       discovered.inaccessible);
        ++completedDirectories;
        const auto saved = catalog.scanSession(sessionId);
        if (saved) {
          report(0.0, std::to_string(saved->files) + " files | " +
                          std::to_string(saved->directories) + " folders | " +
                          std::to_string(saved->pendingDirectories) + " queued | " +
                          directory->string());
        }
        if (completedDirectories % 20 == 0) {
          QMetaObject::invokeMethod(this, [this] { refreshScanSessions(); },
                                    Qt::QueuedConnection);
        }
      }
      catalog.setScanSessionState(sessionId, ScanSessionState::Paused,
                                  "Stopped by user; saved progress can be resumed");
      QMetaObject::invokeMethod(this, [this] { refreshScanSessions(); },
                                Qt::QueuedConnection);
    } catch (const std::exception& error) {
      catalog.setScanSessionState(sessionId, ScanSessionState::Failed, error.what());
      QMetaObject::invokeMethod(this, [this] { refreshScanSessions(); },
                                Qt::QueuedConnection);
      throw;
    }
  });
  scanJobs_.insert(sessionId, jobId);
  refreshScanSessions();
  jobsDock_->show();
}

void MainWindow::refreshScanSessions() {
  if (!scansView_ || !catalog_) return;
  const auto selectedId = scansView_->currentItem()
                              ? scansView_->currentItem()->data(0, Qt::UserRole).toLongLong()
                              : 0;
  scansView_->clear();
  for (const auto& session : catalog_->scanSessions()) {
    auto* item = new QTreeWidgetItem(scansView_);
    item->setData(0, Qt::UserRole, session.id);
    item->setText(0, QString::fromStdString(session.name) +
                         (session.snapshot ? " [snapshot]" : " [live scan]"));
    item->setText(1, QDir::toNativeSeparators(QString::fromStdWString(session.root.wstring())));
    item->setText(2, session.snapshot ? "Saved snapshot" : scanSessionStateName(session.state));
    item->setText(3, QLocale{}.toString(session.files));
    item->setText(4, QLocale{}.toString(session.directories));
    item->setText(5, session.snapshot ? "—" : QLocale{}.toString(session.pendingDirectories));
    item->setText(6, QLocale{}.formattedDataSize(static_cast<qint64>(session.bytes)));
    item->setText(7, QString("Data v%1 / Organizer v%2%3")
                         .arg(session.dataVersion).arg(session.organizationVersion)
                         .arg(session.organizationVersion < PbrOrganizerVersion
                                  ? " - update available" : ""));
    item->setText(8, QLocale{}.toString(
        QDateTime::fromMSecsSinceEpoch(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                session.updatedAt.time_since_epoch()).count()), QLocale::ShortFormat));
    item->setToolTip(0, session.error.empty() ? item->text(1)
                                              : item->text(1) + "\n" +
                                                    QString::fromStdString(session.error));
    if (session.id == selectedId) scansView_->setCurrentItem(item);
  }
}

void MainWindow::pauseSelectedScan() {
  const auto* item = scansView_ ? scansView_->currentItem() : nullptr;
  if (!item) return;
  const auto id = item->data(0, Qt::UserRole).toLongLong();
  const auto session = catalog_->scanSession(id);
  if (!session || session->snapshot) {
    statusBar()->showMessage("A saved snapshot is already frozen and cannot be stopped", 5000);
    return;
  }
  if (const auto job = scanJobs_.find(id); job != scanJobs_.end()) jobs_.cancel(job.value());
  catalog_->setScanSessionState(id, ScanSessionState::Paused,
                                "Stopped by user; saved progress can be resumed");
  refreshScanSessions();
  statusBar()->showMessage("Scan stopped safely. Completed directories and assets remain saved.",
                           6000);
}

void MainWindow::resumeSelectedScan() {
  const auto* item = scansView_ ? scansView_->currentItem() : nullptr;
  if (!item) return;
  const auto id = item->data(0, Qt::UserRole).toLongLong();
  const auto session = catalog_->scanSession(id);
  if (!session) return;
  if (session->snapshot) {
    statusBar()->showMessage("Snapshots are immutable. Re-organize or create a new live scan.", 5000);
    return;
  }
  if (session->state == ScanSessionState::Completed && session->pendingDirectories == 0) {
    statusBar()->showMessage("This scan is already complete", 4000);
    return;
  }
  runPersistentScan(id);
}

void MainWindow::saveSelectedScanAs() {
  const auto* item = scansView_ ? scansView_->currentItem() : nullptr;
  if (!item) {
    statusBar()->showMessage("Select a live scan or an existing snapshot first", 4000);
    return;
  }
  const auto sourceId = item->data(0, Qt::UserRole).toLongLong();
  const auto source = catalog_->scanSession(sourceId);
  if (!source) return;
  bool accepted{};
  const auto defaultName = QString::fromStdString(source->name) + " - " +
                           QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm");
  const auto name = QInputDialog::getText(
      this, "Save scan as", "Snapshot name", QLineEdit::Normal, defaultName, &accepted).trimmed();
  if (!accepted || name.isEmpty()) return;
  try {
    const auto snapshotId = catalog_->saveScanSnapshot(sourceId, name.toStdString());
    refreshScanSessions();
    for (int row = 0; row < scansView_->topLevelItemCount(); ++row) {
      if (scansView_->topLevelItem(row)->data(0, Qt::UserRole).toLongLong() == snapshotId) {
        scansView_->setCurrentItem(scansView_->topLevelItem(row));
        break;
      }
    }
    statusBar()->showMessage(
        QString("Saved immutable scan snapshot: %1. Future organizers can reuse it without rescanning.")
            .arg(name), 8000);
  } catch (const std::exception& error) {
    QMessageBox::critical(this, "Save scan as failed", error.what());
  }
}

void MainWindow::showSelectedScanResults() {
  const auto* item = scansView_ ? scansView_->currentItem() : nullptr;
  if (!item) return;
  showOrganizedScan(item->data(0, Qt::UserRole).toLongLong());
}

void MainWindow::showOrganizedScan(std::int64_t sessionId) {
  const auto session = catalog_->scanSession(sessionId);
  if (!session) return;
  constexpr std::size_t generalDisplayLimit = 10000;
  constexpr std::size_t pbrDisplayLimit = 25000;
  const auto pbrAssets = catalog_->scanPbrTextureAssets(sessionId, pbrDisplayLimit);
  const auto generalAssets = catalog_->scanAssets(sessionId, generalDisplayLimit);
  std::vector<CatalogAsset> assets;
  assets.reserve(pbrAssets.size() + generalAssets.size());
  std::set<AssetId> displayedIds;
  for (const auto& asset : pbrAssets) {
    if (displayedIds.insert(asset.id).second) assets.push_back(asset);
  }
  for (const auto& asset : generalAssets) {
    if (displayedIds.insert(asset.id).second) assets.push_back(asset);
  }
  catalog_->setScanOrganizationVersion(sessionId, PbrOrganizerVersion);
  refreshScanSessions();
  auto* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Organized saved scan");
  dialog->resize(1180, 780);
  auto* layout = new QVBoxLayout(dialog);
  auto* summary = new QLabel(QString(
      "<h2>%1</h2><b>%2</b><br>State: %3 &nbsp; | &nbsp; "
      "%4 files &nbsp; | &nbsp; %5 folders &nbsp; | &nbsp; %6 indexed content"
      " &nbsp; | &nbsp; Data v%7 / Organizer v%8")
      .arg(QString::fromStdString(session->name))
      .arg(QDir::toNativeSeparators(QString::fromStdWString(session->root.wstring())))
      .arg(scanSessionStateName(session->state))
      .arg(QLocale{}.toString(session->files))
      .arg(QLocale{}.toString(session->directories))
      .arg(QLocale{}.formattedDataSize(static_cast<qint64>(session->bytes)))
      .arg(session->dataVersion).arg(PbrOrganizerVersion));
  summary->setTextFormat(Qt::RichText);
  summary->setWordWrap(true);
  layout->addWidget(summary);
  auto* hint = new QLabel(
      "Priority order: PBR materials, 3D files, images, video, archives, audio, "
      "documents, then other files. Double-click an asset to open its folder.");
  hint->setWordWrap(true);
  layout->addWidget(hint);

  auto* tree = new QTreeWidget;
  tree->setHeaderLabels({"Organized asset", "Role / type", "Size", "Location"});
  tree->setAlternatingRowColors(true);
  tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
  auto* pbrRoot = new QTreeWidgetItem(tree, {"1. PBR materials"});
  auto* modelsRoot = new QTreeWidgetItem(tree, {"2. 3D files"});
  auto* imagesRoot = new QTreeWidgetItem(tree, {"3. Images and textures"});
  auto* videoRoot = new QTreeWidgetItem(tree, {"4. Video"});
  auto* archiveRoot = new QTreeWidgetItem(tree, {"5. Archives"});
  auto* audioRoot = new QTreeWidgetItem(tree, {"6. Audio"});
  auto* documentsRoot = new QTreeWidgetItem(tree, {"7. Documents"});
  auto* otherRoot = new QTreeWidgetItem(tree, {"8. Other files"});
  QHash<QString, QTreeWidgetItem*> materialGroups;
  QHash<QString, QSet<QString>> materialRoles;
  QHash<QString, std::set<PbrMapKind>> materialMapKinds;
  QHash<QString, QHash<QString, QSet<QString>>> materialVariantEvidence;
  for (const auto& asset : assets) {
    if (asset.metadata.isDirectory || classifyAsset(asset.metadata.path) != AssetKind::Image) continue;
    const auto identity = identifyPbrTexture(asset.metadata.path);
    if (!identity || identity->canonicalMaterialKey.empty() || identity->mapVariant.empty()) continue;
    const auto path = QString::fromStdWString(asset.metadata.path.wstring());
    const auto baseKey = QFileInfo(path).absolutePath().toLower() + "|" +
                         QString::fromStdString(identity->canonicalMaterialKey);
    materialVariantEvidence[baseKey][QString::fromStdString(identity->mapVariant)].insert(
        QString::fromUtf8(pbrMapKindName(identity->kind)));
  }
  for (const auto& asset : assets) {
    if (asset.metadata.isDirectory) continue;
    const auto path = QString::fromStdWString(asset.metadata.path.wstring());
    const QFileInfo info(path);
    const auto kind = classifyAsset(asset.metadata.path);
    const auto pbrIdentity = kind == AssetKind::Image
                                 ? identifyPbrTexture(asset.metadata.path)
                                 : std::optional<PbrTextureIdentity>{};
    QTreeWidgetItem* parent{};
    QString role = QString::fromUtf8(assetKindName(kind).data());
    if (pbrIdentity) {
      const bool folderIdentity = pbrIdentity->canonicalMaterialKey.empty();
      auto materialName = folderIdentity ? info.dir().dirName()
                                         : QString::fromStdString(pbrIdentity->materialName);
      const auto baseMaterialKey = info.absolutePath().toLower() + "|" +
                                   (folderIdentity ? "@dedicated-folder"
                                                   : QString::fromStdString(
                                                         pbrIdentity->canonicalMaterialKey));
      const auto variant = QString::fromStdString(pbrIdentity->mapVariant);
      const bool provenVariant = !variant.isEmpty() &&
          materialVariantEvidence.value(baseMaterialKey).value(variant).size() >= 2;
      const auto materialKey = provenVariant ? baseMaterialKey + "|variant:" + variant
                                             : baseMaterialKey;
      if (provenVariant) materialName += " - variant " + variant;
      parent = materialGroups.value(materialKey);
      if (!parent) {
        parent = new QTreeWidgetItem(pbrRoot, {materialName, "Detected material"});
        parent->setToolTip(0, info.absolutePath());
        materialGroups.insert(materialKey, parent);
      }
      role = QString::fromUtf8(pbrMapKindName(pbrIdentity->kind));
      materialRoles[materialKey].insert(role);
      materialMapKinds[materialKey].insert(pbrIdentity->kind);
    } else {
      switch (kind) {
        case AssetKind::Model: parent = modelsRoot; break;
        case AssetKind::Image: parent = imagesRoot; break;
        case AssetKind::Video: parent = videoRoot; break;
        case AssetKind::Archive: parent = archiveRoot; break;
        case AssetKind::Audio: parent = audioRoot; break;
        case AssetKind::Document: parent = documentsRoot; break;
        case AssetKind::Other: parent = otherRoot; break;
        case AssetKind::Directory: continue;
      }
    }
    auto* item = new QTreeWidgetItem(parent,
        {info.fileName(), role,
         QLocale{}.formattedDataSize(static_cast<qint64>(asset.metadata.sizeBytes)), path});
    item->setData(0, Qt::UserRole, path);
  }
  QStringList rejectedMaterialKeys;
  for (auto iterator = materialGroups.cbegin(); iterator != materialGroups.cend(); ++iterator) {
    if (materialMapKinds.value(iterator.key()).size() < 2) {
      auto* group = iterator.value();
      pbrRoot->removeChild(group);
      imagesRoot->addChildren(group->takeChildren());
      delete group;
      rejectedMaterialKeys.append(iterator.key());
    }
  }
  for (const auto& key : rejectedMaterialKeys) {
    materialGroups.remove(key);
    materialRoles.remove(key);
    materialMapKinds.remove(key);
  }
  for (auto iterator = materialGroups.cbegin(); iterator != materialGroups.cend(); ++iterator) {
    const auto roles = materialRoles.value(iterator.key());
    const auto workflow = classifyPbrWorkflow(materialMapKinds.value(iterator.key()));
    const bool complete = workflow != PbrWorkflow::Incomplete;
    iterator.value()->setText(1, QString::fromUtf8(pbrWorkflowName(workflow)));
    iterator.value()->setForeground(1, complete ? QColor("#78d89a") : QColor("#e2b66d"));
    QStringList detectedRoles;
    for (const auto& role : roles) detectedRoles.append(role);
    detectedRoles.sort(Qt::CaseInsensitive);
    iterator.value()->setToolTip(1, detectedRoles.join(", "));
  }
  pbrRoot->setText(0, QString("1. PBR materials (%1 sets)").arg(materialGroups.size()));
  modelsRoot->setText(0, QString("2. 3D files (%1)").arg(modelsRoot->childCount()));
  imagesRoot->setText(0, QString("3. Images and textures (%1)").arg(imagesRoot->childCount()));
  videoRoot->setText(0, QString("4. Video (%1)").arg(videoRoot->childCount()));
  archiveRoot->setText(0, QString("5. Archives (%1)").arg(archiveRoot->childCount()));
  audioRoot->setText(0, QString("6. Audio (%1)").arg(audioRoot->childCount()));
  documentsRoot->setText(0, QString("7. Documents (%1)").arg(documentsRoot->childCount()));
  otherRoot->setText(0, QString("8. Other files (%1)").arg(otherRoot->childCount()));
  pbrRoot->setExpanded(true);
  modelsRoot->setExpanded(true);
  connect(tree, &QTreeWidget::itemDoubleClicked, dialog,
          [this](QTreeWidgetItem* item, int) {
    const auto path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty() && currentTab()) {
      currentTab()->activePane()->navigate(QFileInfo(path).absolutePath());
    }
  });
  layout->addWidget(tree, 1);
  if (session->files > assets.size()) {
    auto* limited = new QLabel(QString(
        "Showing %1 prioritized assets, including up to %2 texture-map candidates. "
        "All %3 discovered files remain saved and searchable in the Atlas catalog.")
        .arg(QLocale{}.toString(assets.size())).arg(QLocale{}.toString(pbrDisplayLimit))
        .arg(QLocale{}.toString(session->files)));
    limited->setWordWrap(true);
    layout->addWidget(limited);
  }
  auto* buttons = new QHBoxLayout;
  auto* resume = new QPushButton("Resume scan");
  resume->setEnabled(!session->snapshot && session->state != ScanSessionState::Completed);
  auto* close = new QPushButton("Close");
  buttons->addWidget(resume);
  buttons->addStretch();
  buttons->addWidget(close);
  connect(resume, &QPushButton::clicked, dialog, [this, sessionId] {
    runPersistentScan(sessionId);
  });
  connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
  layout->addLayout(buttons);
  dialog->show();
}

void MainWindow::showStatistics() {
  if (!currentTab()) return;
  analyzePath(currentTab()->activePane()->path(), "Folder scan", true);
}

void MainWindow::showDriveStatistics() {
  if (!currentTab()) return;
  const QStorageInfo storage(currentTab()->activePane()->path());
  if (!storage.isValid() || !storage.isReady()) {
    QMessageBox::warning(this, "Scan current drive", "Atlas could not determine the current drive.");
    return;
  }
  const auto root = storage.rootPath();
  if (QMessageBox::question(this, "Scan current drive",
      "Recursively scan " + QDir::toNativeSeparators(root) +
      "?\n\nAtlas will save every discovery and its progress. You can stop the scan and "
      "resume it later, including after restarting Atlas. Originals remain untouched.") != QMessageBox::Yes) return;
  startPersistentScan(root, true);
}

void MainWindow::analyzePath(const QString& path, const QString& title, bool findDuplicates) {
  const auto root = std::filesystem::path(path.toStdWString());
  statusBar()->showMessage(title + " started — progress is shown in Background jobs", 6000);
  jobsDock_->show();
  jobs_.submit(title.toStdString() + " " + root.string(), [this, root, title, findDuplicates](std::stop_token token, const JobReporter& report) {
    StorageAnalyzer analyzer;
    auto analysis = analyzer.analyze(root, token, 100, findDuplicates,
        [&report](std::uint64_t files, std::uint64_t folders, const auto& path) {
      report(0.0, std::to_string(files) + " files | " +
                      std::to_string(folders) + " folders | " + path.string());
    });
    if (analysis.cancelled) return;
    QMetaObject::invokeMethod(this,
        [this, root, title, findDuplicates, analysis = std::move(analysis)]() mutable {
      showAnalysisResults(root, title, findDuplicates, std::move(analysis));
    }, Qt::QueuedConnection);
  });
}

void MainWindow::showAnalysisResults(const std::filesystem::path& root, const QString& title,
                                     bool findDuplicates, StorageAnalysis analysis) {
  const auto formattedSize = [](std::uintmax_t bytes) {
    return QLocale{}.formattedDataSize(static_cast<qint64>(bytes));
  };
  const auto percentage = [](std::uintmax_t value, std::uintmax_t total) {
    return total == 0 ? QString("0.00%")
                      : QString::number(100.0 * static_cast<double>(value) /
                                            static_cast<double>(total), 'f', 2) + "%";
  };
  const auto configureTable = [](QTableWidget* table) {
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
  };
  const auto pathString = [](const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
  };

  auto* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(title + " - Drive Intelligence");
  dialog->resize(1240, 820);
  auto* layout = new QVBoxLayout(dialog);
  auto* heading = new QLabel(QString(
      "<h2>Drive Intelligence</h2><b>%1</b><br>"
      "%2 files in %3 folders - %4 scanned - %5 PBR material sets - "
      "%6 FBX files - %7 archives")
      .arg(QDir::toNativeSeparators(pathString(root)))
      .arg(analysis.files).arg(analysis.directories).arg(formattedSize(analysis.bytes))
      .arg(analysis.pbrMaterials.size()).arg(analysis.fbxFiles.size())
      .arg(analysis.archiveFiles.size()));
  heading->setTextFormat(Qt::RichText);
  heading->setWordWrap(true);
  layout->addWidget(heading);

  auto* resultTabs = new QTabWidget;
  layout->addWidget(resultTabs, 1);

  auto* overview = new QWidget;
  auto* overviewLayout = new QVBoxLayout(overview);
  auto* driveSummary = new QLabel(QString(
      "Drive used: %1 of %2 (%3 available)    |    Empty folders: %4    |    "
      "Duplicate groups: %5%6")
      .arg(formattedSize(analysis.capacity - analysis.available))
      .arg(formattedSize(analysis.capacity)).arg(formattedSize(analysis.available))
      .arg(analysis.emptyFolderCount)
      .arg(findDuplicates ? QString::number(analysis.duplicates.size()) : "not checked")
      .arg(findDuplicates ? QString{} : " (disabled for whole-drive speed)"));
  overviewLayout->addWidget(driveSummary);
  auto* categoryTable = new QTableWidget(7, 5);
  categoryTable->setHorizontalHeaderLabels(
      {"Category", "Files", "% of files", "Size", "% of scanned bytes"});
  configureTable(categoryTable);
  struct CategoryRow { const char* name; std::uint64_t files; std::uint64_t bytes; };
  const std::array categories{
      CategoryRow{"Images", analysis.assetTypes.images, analysis.assetBytes.images},
      CategoryRow{"3D models", analysis.assetTypes.models, analysis.assetBytes.models},
      CategoryRow{"Videos", analysis.assetTypes.videos, analysis.assetBytes.videos},
      CategoryRow{"Audio", analysis.assetTypes.audio, analysis.assetBytes.audio},
      CategoryRow{"Archives", analysis.assetTypes.archives, analysis.assetBytes.archives},
      CategoryRow{"Documents", analysis.assetTypes.documents, analysis.assetBytes.documents},
      CategoryRow{"Other", analysis.assetTypes.other, analysis.assetBytes.other}};
  const std::array categoryColors{
      QColor("#3979ad"), QColor("#7055ad"), QColor("#b04c68"), QColor("#3b9a86"),
      QColor("#c07b35"), QColor("#65813c"), QColor("#626b78")};
  for (int row = 0; row < static_cast<int>(categories.size()); ++row) {
    const auto& category = categories[static_cast<std::size_t>(row)];
    categoryTable->setItem(row, 0, new QTableWidgetItem(category.name));
    categoryTable->setItem(row, 1, new QTableWidgetItem(QString::number(category.files)));
    categoryTable->setItem(row, 2, new QTableWidgetItem(percentage(category.files, analysis.files)));
    categoryTable->setItem(row, 3, new QTableWidgetItem(formattedSize(category.bytes)));
    categoryTable->setItem(row, 4, new QTableWidgetItem(percentage(category.bytes, analysis.bytes)));
    for (int column = 0; column < categoryTable->columnCount(); ++column) {
      categoryTable->item(row, column)->setBackground(categoryColors[static_cast<std::size_t>(row)]);
    }
  }
  categoryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  overviewLayout->addWidget(categoryTable, 1);
  auto* interpretation = new QLabel(
      "Percentages are calculated from exact recognized files and their scanned sizes. "
      "Unknown extensions remain visible under Other and in File types.");
  interpretation->setWordWrap(true);
  overviewLayout->addWidget(interpretation);
  resultTabs->addTab(overview, "Overview");

  auto* heatmap = new QTableWidget(static_cast<int>(analysis.largestFiles.size()), 4);
  heatmap->setHorizontalHeaderLabels({"Rank", "Largest file", "Size", "% of scanned data"});
  configureTable(heatmap);
  heatmap->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  const auto largest = analysis.largestFiles.empty() ? 1 : analysis.largestFiles.front().bytes;
  for (int row = 0; row < static_cast<int>(analysis.largestFiles.size()); ++row) {
    const auto& entry = analysis.largestFiles[static_cast<std::size_t>(row)];
    const auto relative = static_cast<double>(entry.bytes) / static_cast<double>(largest);
    const auto hue = static_cast<int>(115.0 * (1.0 - relative));
    const QColor heatColor = QColor::fromHsv(hue, 150, 135);
    heatmap->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
    auto* fileItem = new QTableWidgetItem(pathString(entry.path));
    fileItem->setData(Qt::UserRole, pathString(entry.path));
    heatmap->setItem(row, 1, fileItem);
    heatmap->setItem(row, 2, new QTableWidgetItem(formattedSize(entry.bytes)));
    heatmap->setItem(row, 3, new QTableWidgetItem(percentage(entry.bytes, analysis.bytes)));
    for (int column = 0; column < heatmap->columnCount(); ++column) {
      heatmap->item(row, column)->setBackground(heatColor);
    }
  }
  connect(heatmap, &QTableWidget::cellDoubleClicked, dialog, [this, heatmap](int row, int) {
    const auto path = heatmap->item(row, 1)->data(Qt::UserRole).toString();
    if (currentTab()) currentTab()->activePane()->navigate(QFileInfo(path).absolutePath());
  });
  resultTabs->addTab(heatmap, "Largest-file heatmap");

  auto* pbrTable = new QTableWidget(static_cast<int>(analysis.pbrMaterials.size()), 5);
  pbrTable->setHorizontalHeaderLabels({"Material", "Status", "Maps", "Map count", "Folder"});
  configureTable(pbrTable);
  pbrTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  pbrTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  pbrTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  for (int row = 0; row < static_cast<int>(analysis.pbrMaterials.size()); ++row) {
    const auto& material = analysis.pbrMaterials[static_cast<std::size_t>(row)];
    QStringList maps;
    QStringList texturePaths;
    std::set<PbrMapKind> uniqueKinds;
    for (const auto& texture : material.textures) {
      uniqueKinds.insert(texture.kind);
      texturePaths.append(pathString(texture.path));
    }
    for (const auto kind : uniqueKinds) maps.append(pbrMapKindName(kind));
    auto* name = new QTableWidgetItem(QString::fromStdString(material.name));
    name->setToolTip(texturePaths.join('\n'));
    pbrTable->setItem(row, 0, name);
    auto* status = new QTableWidgetItem(
        material.hasCoreMaps ? "Core set complete" : "Candidate / incomplete");
    status->setForeground(material.hasCoreMaps ? QColor("#78d89a") : QColor("#e2b66d"));
    pbrTable->setItem(row, 1, status);
    pbrTable->setItem(row, 2, new QTableWidgetItem(maps.join(", ")));
    pbrTable->setItem(row, 3, new QTableWidgetItem(QString::number(material.textures.size())));
    auto* folder = new QTableWidgetItem(pathString(material.directory));
    folder->setData(Qt::UserRole, pathString(material.directory));
    pbrTable->setItem(row, 4, folder);
  }
  connect(pbrTable, &QTableWidget::cellDoubleClicked, dialog, [this, pbrTable](int row, int) {
    if (currentTab()) currentTab()->activePane()->navigate(
        pbrTable->item(row, 4)->data(Qt::UserRole).toString());
  });
  resultTabs->addTab(pbrTable,
                     QString("PBR materials (%1)").arg(analysis.pbrMaterials.size()));

  auto* organization = new QSplitter(Qt::Vertical);
  auto* typeTable = new QTableWidget(static_cast<int>(analysis.fileTypes.size()), 5);
  typeTable->setHorizontalHeaderLabels(
      {"Extension", "Files", "% of files", "Size", "% of scanned bytes"});
  configureTable(typeTable);
  typeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int row = 0; row < static_cast<int>(analysis.fileTypes.size()); ++row) {
    const auto& type = analysis.fileTypes[static_cast<std::size_t>(row)];
    typeTable->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(type.extension)));
    typeTable->setItem(row, 1, new QTableWidgetItem(QString::number(type.files)));
    typeTable->setItem(row, 2, new QTableWidgetItem(percentage(type.files, analysis.files)));
    typeTable->setItem(row, 3, new QTableWidgetItem(formattedSize(type.bytes)));
    typeTable->setItem(row, 4, new QTableWidgetItem(percentage(type.bytes, analysis.bytes)));
  }
  organization->addWidget(typeTable);
  auto* inventory = new QTreeWidget;
  inventory->setHeaderLabels({"Organized finding", "Size", "Location"});
  auto* fbxGroup = new QTreeWidgetItem(inventory,
      {QString("FBX models (%1)").arg(analysis.fbxFiles.size())});
  for (const auto& entry : analysis.fbxFiles) {
    auto* item = new QTreeWidgetItem(fbxGroup,
        {QFileInfo(pathString(entry.path)).fileName(), formattedSize(entry.bytes),
         pathString(entry.path)});
    item->setData(0, Qt::UserRole, pathString(entry.path));
  }
  auto* archiveGroup = new QTreeWidgetItem(inventory,
      {QString("Archives: ZIP, 7Z, RAR and others (%1)").arg(analysis.archiveFiles.size())});
  for (const auto& entry : analysis.archiveFiles) {
    auto* item = new QTreeWidgetItem(archiveGroup,
        {QFileInfo(pathString(entry.path)).fileName(), formattedSize(entry.bytes),
         pathString(entry.path)});
    item->setData(0, Qt::UserRole, pathString(entry.path));
  }
  inventory->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  inventory->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  connect(inventory, &QTreeWidget::itemDoubleClicked, dialog, [this](QTreeWidgetItem* item, int) {
    const auto path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty() && currentTab()) {
      currentTab()->activePane()->navigate(QFileInfo(path).absolutePath());
    }
  });
  organization->addWidget(inventory);
  organization->setStretchFactor(0, 1);
  organization->setStretchFactor(1, 1);
  resultTabs->addTab(organization, "File types and findings");

  auto* cleanup = new QTreeWidget;
  cleanup->setHeaderLabels({"Diagnostic", "Potential reclaim", "Location"});
  auto* duplicateRoot = new QTreeWidgetItem(cleanup,
      {findDuplicates ? QString("Exact duplicate groups (%1)").arg(analysis.duplicates.size())
                      : "Exact duplicates were not checked for this drive scan"});
  for (const auto& group : analysis.duplicates) {
    auto* groupItem = new QTreeWidgetItem(duplicateRoot,
        {QString("%1 identical files").arg(group.paths.size()),
         formattedSize(group.bytesPerFile * (group.paths.size() - 1))});
    for (const auto& path : group.paths) {
      auto* item = new QTreeWidgetItem(groupItem,
          {QFileInfo(pathString(path)).fileName(), formattedSize(group.bytesPerFile),
           pathString(path)});
      item->setData(0, Qt::UserRole, pathString(path));
    }
  }
  auto* emptyRoot = new QTreeWidgetItem(cleanup,
      {QString("Empty folders (%1; first %2 shown)")
           .arg(analysis.emptyFolderCount).arg(analysis.emptyFolders.size())});
  for (const auto& path : analysis.emptyFolders) {
    auto* item = new QTreeWidgetItem(emptyRoot,
        {QFileInfo(pathString(path)).fileName(), {}, pathString(path)});
    item->setData(0, Qt::UserRole, pathString(path));
  }
  cleanup->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  cleanup->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  connect(cleanup, &QTreeWidget::itemDoubleClicked, dialog, [this](QTreeWidgetItem* item, int) {
    const auto path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty() && currentTab()) {
      currentTab()->activePane()->navigate(
          QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath());
    }
  });
  resultTabs->addTab(cleanup, "Cleanup diagnostics");

  auto* close = new QPushButton("Close");
  connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  buttons->addWidget(close);
  layout->addLayout(buttons);
  dialog->show();
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
  enqueueTransfer(paths, destination, move, answer == QMessageBox::Yes);
}

void MainWindow::enqueueTransfer(const QStringList& paths, const QString& destination,
                                 bool move, bool renameConflicts) {
  std::vector<std::filesystem::path> sources;
  for (const auto& path : paths) {
    if (move && QDir::cleanPath(QFileInfo(path).absolutePath()) == QDir::cleanPath(destination)) continue;
    sources.emplace_back(path.toStdWString());
  }
  if (sources.empty()) {
    statusBar()->showMessage("The selected items are already in this folder", 4000);
    return;
  }
  jobsDock_->show();
  statusBar()->showMessage(QString("%1 %2 item(s) to %3")
                               .arg(move ? "Moving" : "Copying")
                               .arg(sources.size())
                               .arg(QDir::toNativeSeparators(destination)), 6000);
  const auto sourceCount = sources.size();
  jobs_.submit((move ? "Move " : "Copy ") + std::to_string(sourceCount) + " item(s)",
      [sources = std::move(sources), target = std::filesystem::path(destination.toStdWString()), move, renameConflicts](std::stop_token token, const JobReporter& report) {
        const auto result = FileOperations::transfer(move ? FileOperationKind::Move : FileOperationKind::Copy, sources, target,
            renameConflicts ? ConflictPolicy::Rename : ConflictPolicy::Skip, token,
            [&report](const FileOperationProgress& progress) { report(progress.total == 0 ? 1.0 : static_cast<double>(progress.completed) / progress.total, progress.source.string()); });
        if (!result.failed.empty()) throw std::runtime_error(std::to_string(result.failed.size()) + " item(s) failed");
      });
}

void MainWindow::rememberSelection(bool move) {
  if (!currentTab()) return;
  transferClipboard_ = currentTab()->activePane()->selectedPaths();
  transferClipboardMoves_ = move;
  pasteAction_->setEnabled(!transferClipboard_.isEmpty());
  if (!transferClipboard_.isEmpty()) {
    statusBar()->showMessage(QString("%1 item(s) ready to %2 — open the destination and choose Paste here")
                                 .arg(transferClipboard_.size()).arg(move ? "move" : "copy"), 6000);
  }
}

void MainWindow::pasteHere() {
  if (!currentTab() || transferClipboard_.isEmpty()) return;
  enqueueTransfer(transferClipboard_, currentTab()->activePane()->path(), transferClipboardMoves_, true);
  if (transferClipboardMoves_) {
    transferClipboard_.clear();
    pasteAction_->setEnabled(false);
  }
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
