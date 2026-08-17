#include <AtlasDatabase/Catalog.h>
#include <AtlasCore/FileOperations.h>
#include <AtlasCore/PbrMaterial.h>
#include <AtlasCore/StorageAnalyzer.h>
#include <AtlasJobs/JobQueue.h>
#include <AtlasScanner/Scanner.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("atlas-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }
 private:
  std::filesystem::path path_;
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void catalogPreservesIdentity() {
  TemporaryDirectory temporary;
  atlas::Catalog catalog(temporary.path() / "catalog.db");
  atlas::AssetMetadata metadata{
      .path = temporary.path() / "image.exr",
      .sizeBytes = 512,
      .modifiedAt = std::chrono::system_clock::now(),
      .isDirectory = false};
  const auto firstId = catalog.upsert(metadata);
  metadata.sizeBytes = 1024;
  const auto secondId = catalog.upsert(metadata);
  require(firstId == secondId, "upsert changed stable asset ID");
  require(catalog.assetCount() == 1, "upsert duplicated an asset");
  const auto stored = catalog.findByPath(metadata.path);
  require(stored.has_value(), "stored asset was not found");
  require(stored->metadata.sizeBytes == 1024, "stored metadata was not refreshed");
}

void catalogSearchesAndMaintainsState() {
  TemporaryDirectory temporary;
  atlas::Catalog catalog(temporary.path() / "catalog.db");
  const auto now = std::chrono::system_clock::now();
  const std::vector<atlas::AssetMetadata> assets{
      {.path = temporary.path() / "textures", .modifiedAt = now, .isDirectory = true},
      {.path = temporary.path() / "textures" / "hero_albedo.png", .sizeBytes = 4096, .modifiedAt = now},
      {.path = temporary.path() / "textures" / "hero_normal.exr", .sizeBytes = 8192, .modifiedAt = now},
      {.path = temporary.path() / "notes.txt", .sizeBytes = 32, .modifiedAt = now}};
  const auto ids = catalog.upsertBatch(assets);
  require(ids.size() == assets.size(), "batch upsert did not return all IDs");
  atlas::AssetQuery query;
  query.text = "hero";
  query.extension = "png";
  query.beneath = temporary.path();
  const auto matches = catalog.search(query);
  require(matches.size() == 1, "catalog filters returned the wrong results");
  require(matches.front().metadata.sizeBytes == 4096, "catalog search returned wrong asset");
  const auto statistics = catalog.statistics();
  require(statistics.files == 3 && statistics.directories == 1 && statistics.bytes == 12320,
          "catalog statistics are incorrect");
  require(catalog.largestFiles(1).front().metadata.sizeBytes == 8192,
          "largest file query is incorrect");
  catalog.addBookmark(temporary.path() / "textures");
  require(catalog.bookmarks().size() == 1, "bookmark was not stored");
  catalog.removeBookmark(temporary.path() / "textures");
  require(catalog.bookmarks().empty(), "bookmark was not removed");
  const std::vector<std::filesystem::path> observed{assets[0].path, assets[1].path, assets[3].path};
  require(catalog.removeMissingUnder(temporary.path(), observed) == 1,
          "stale catalog entry was not removed");
}

void scannerReportsContents() {
  TemporaryDirectory temporary;
  std::filesystem::create_directory(temporary.path() / "textures");
  std::ofstream(temporary.path() / "textures" / "albedo.png") << "pixels";
  std::ofstream(temporary.path() / "notes.txt") << "atlas";
  std::uint64_t callbacks{};
  const auto summary = atlas::Scanner{}.scan(
      temporary.path(), std::stop_token{},
      [&callbacks](const atlas::AssetMetadata&) { ++callbacks; });
  require(summary.files == 2, "scanner file count is incorrect");
  require(summary.directories == 1, "scanner directory count is incorrect");
  require(summary.bytes == 11, "scanner byte count is incorrect");
  require(callbacks == 3, "scanner did not report every entry");
  require(!summary.cancelled, "scanner unexpectedly cancelled");
}

void scannerHonorsCancellation() {
  TemporaryDirectory temporary;
  std::ofstream(temporary.path() / "file.txt") << "content";
  std::stop_source source;
  source.request_stop();
  const auto summary = atlas::Scanner{}.scan(
      temporary.path(), source.get_token(), [](const atlas::AssetMetadata&) {});
  require(summary.cancelled, "scanner ignored cancellation");
  require(summary.files == 0, "cancelled scanner processed files");
}

void scannerHonorsDepth() {
  TemporaryDirectory temporary;
  std::filesystem::create_directories(temporary.path() / "one" / "two");
  std::ofstream(temporary.path() / "one" / "top.txt") << "top";
  std::ofstream(temporary.path() / "one" / "two" / "deep.txt") << "deep";
  atlas::ScanOptions options;
  options.maximumDepth = 0;
  const auto summary = atlas::Scanner{}.scan(temporary.path(), {}, [](const auto&) {}, options);
  require(summary.directories == 1, "depth-limited scanner missed first level");
  require(summary.files == 0, "depth-limited scanner recursed too far");
}

void persistentScanCanStopAndResume() {
  TemporaryDirectory temporary;
  const auto root = temporary.path() / "library";
  std::filesystem::create_directories(root / "materials");
  std::ofstream(root / "hero.fbx") << "model";
  std::ofstream(root / "materials" / "stone_albedo.png") << "color";
  std::ofstream(root / "materials" / "stone_normal.png") << "normal";

  const auto database = temporary.path() / "catalog.db";
  atlas::ScanSessionId sessionId{};
  atlas::ScanSessionId snapshotId{};
  {
    atlas::Catalog catalog(database);
    sessionId = catalog.createScanSession(root, false);
    catalog.setScanSessionState(sessionId, atlas::ScanSessionState::Running);
    const auto pending = catalog.nextScanDirectory(sessionId);
    require(pending && atlas::normalizedPath(*pending) == atlas::normalizedPath(root),
            "persistent scan did not queue its root");
    const auto directory = atlas::Scanner{}.scanDirectory(*pending);
    catalog.recordScannedDirectory(sessionId, *pending, directory.entries,
                                   directory.inaccessible);
    catalog.setScanSessionState(sessionId, atlas::ScanSessionState::Paused);
    const auto paused = catalog.scanSession(sessionId);
    require(paused && paused->state == atlas::ScanSessionState::Paused,
            "paused scan state was not saved");
    require(paused->files == 1 && paused->directories == 1 &&
                paused->pendingDirectories == 1,
            "partial scan totals or checkpoint are incorrect");
    snapshotId = catalog.saveScanSnapshot(sessionId, "Library checkpoint");
    const auto snapshot = catalog.scanSession(snapshotId);
    require(snapshot && snapshot->snapshot && snapshot->name == "Library checkpoint" &&
                snapshot->sourceSessionId == sessionId && snapshot->files == 1 &&
                snapshot->directories == 1 && snapshot->pendingDirectories == 0,
            "Save scan as did not create an immutable named snapshot");
  }
  {
    atlas::Catalog catalog(database);
    const auto restored = catalog.scanSession(sessionId);
    require(restored && restored->state == atlas::ScanSessionState::Paused,
            "scan session did not survive catalog reopen");
    catalog.setScanSessionState(sessionId, atlas::ScanSessionState::Running);
    const auto pending = catalog.nextScanDirectory(sessionId);
    require(pending && pending->filename() == "materials",
            "scan did not resume at the pending directory");
    const auto directory = atlas::Scanner{}.scanDirectory(*pending);
    catalog.recordScannedDirectory(sessionId, *pending, directory.entries,
                                   directory.inaccessible);
    require(!catalog.nextScanDirectory(sessionId), "completed scan retained pending work");
    catalog.setScanSessionState(sessionId, atlas::ScanSessionState::Completed);
    const auto complete = catalog.scanSession(sessionId);
    require(complete && complete->files == 3 && complete->directories == 1 &&
                complete->pendingDirectories == 0,
            "resumed scan totals are incorrect");
    const auto assets = catalog.scanAssets(sessionId);
    require(assets.size() == 4, "saved scan did not retain every discovered asset");
    require(assets.front().metadata.path.filename() == "stone_albedo.png",
            "organized scan results did not prioritize PBR textures");
    const auto snapshotAssets = catalog.scanAssets(snapshotId);
    require(snapshotAssets.size() == 2,
            "saved scan snapshot changed when its source scan resumed");
    atlas::AssetMetadata changedHero{
        .path = root / "hero.fbx",
        .sizeBytes = 999,
        .modifiedAt = std::chrono::system_clock::now()};
    catalog.upsert(changedHero);
    const auto frozenSnapshotAssets = catalog.scanAssets(snapshotId);
    const auto frozenHero = std::find_if(frozenSnapshotAssets.begin(), frozenSnapshotAssets.end(),
        [](const auto& asset) { return asset.metadata.path.filename() == "hero.fbx"; });
    require(frozenHero != frozenSnapshotAssets.end() && frozenHero->metadata.sizeBytes == 5,
            "saved scan snapshot metadata changed with the live catalog");
    catalog.setScanOrganizationVersion(snapshotId, atlas::PbrOrganizerVersion);
    const auto reorganized = catalog.scanSession(snapshotId);
    require(reorganized &&
                reorganized->organizationVersion == atlas::PbrOrganizerVersion &&
                reorganized->dataVersion == 1,
            "saved scan did not record independent data and organizer versions");
  }
}

void fileOperationsAreConflictAware() {
  TemporaryDirectory temporary;
  const auto sourceDirectory = temporary.path() / "source";
  const auto targetDirectory = temporary.path() / "target";
  std::filesystem::create_directories(sourceDirectory);
  std::filesystem::create_directories(targetDirectory);
  const auto source = sourceDirectory / "asset.txt";
  std::ofstream(source) << "atlas";
  auto result = atlas::FileOperations::transfer(atlas::FileOperationKind::Copy, {source},
      targetDirectory, atlas::ConflictPolicy::Skip);
  require(result.succeeded == 1 && std::filesystem::exists(targetDirectory / "asset.txt"),
          "copy operation failed");
  result = atlas::FileOperations::transfer(atlas::FileOperationKind::Copy, {source},
      targetDirectory, atlas::ConflictPolicy::Rename);
  require(result.succeeded == 1 && std::filesystem::exists(targetDirectory / "asset (2).txt"),
          "rename-on-conflict failed");
  const auto duplicate = atlas::FileOperations::duplicate(source);
  require(std::filesystem::exists(duplicate), "duplicate operation failed");
  const auto renamed = sourceDirectory / "renamed.txt";
  atlas::FileOperations::rename(source, renamed);
  require(std::filesystem::exists(renamed), "rename operation failed");
  const auto moveSource = sourceDirectory / "move.txt";
  std::ofstream(moveSource) << "move me";
  result = atlas::FileOperations::transfer(atlas::FileOperationKind::Move, {moveSource},
      targetDirectory, atlas::ConflictPolicy::Skip);
  require(result.succeeded == 1 && !std::filesystem::exists(moveSource) &&
              std::filesystem::exists(targetDirectory / "move.txt"),
          "move operation failed");
}

void jobQueueRunsAndCancelsWork() {
  atlas::JobQueue queue(2);
  std::atomic<int> completed{};
  const auto first = queue.submit("complete", [&completed](std::stop_token, const atlas::JobReporter& report) {
    report(0.5, "half");
    ++completed;
  });
  const auto second = queue.submit("cancel", [](std::stop_token token, const atlas::JobReporter&) {
    while (!token.stop_requested()) std::this_thread::yield();
  });
  require(queue.cancel(second), "running or queued job could not be cancelled");
  require(queue.waitForIdle(std::chrono::seconds(5)), "job queue did not become idle");
  require(completed == 1, "job queue did not execute work");
  const auto snapshots = queue.snapshots();
  require(snapshots.at(static_cast<std::size_t>(first - 1)).state == atlas::JobState::Completed,
          "completed job has wrong state");
  require(snapshots.at(static_cast<std::size_t>(second - 1)).state == atlas::JobState::Cancelled,
          "cancelled job has wrong state");
}

void jobQueueShutdownCancelsRunningWork() {
  std::atomic<bool> started{};
  std::atomic<bool> stopped{};
  {
    atlas::JobQueue queue(1);
    queue.submit("shutdown", [&started, &stopped](std::stop_token token,
                                                   const atlas::JobReporter&) {
      started = true;
      while (!token.stop_requested()) std::this_thread::yield();
      stopped = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!started && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(started, "shutdown test job did not start");
  }
  require(stopped, "job queue shutdown did not cancel running work");
}

void assetKindsAreRecognized() {
  require(atlas::classifyAsset("texture.EXR") == atlas::AssetKind::Image,
          "image classification is not case insensitive");
  require(atlas::classifyAsset("mesh.fbx") == atlas::AssetKind::Model,
          "model classification failed");
  require(atlas::classifyAsset("scene.MAX") == atlas::AssetKind::Model,
          "DCC model classification failed");
  require(atlas::classifyAsset("render.mov") == atlas::AssetKind::Video,
          "video classification failed");
  require(atlas::classifyAsset("folder", true) == atlas::AssetKind::Directory,
          "directory classification failed");
}

void storageAnalysisFindsWaste() {
  TemporaryDirectory temporary;
  std::filesystem::create_directories(temporary.path() / "empty");
  std::filesystem::create_directories(temporary.path() / "assets");
  std::ofstream(temporary.path() / "assets" / "first.bin") << "duplicate content";
  std::ofstream(temporary.path() / "assets" / "second.bin") << "duplicate content";
  std::ofstream(temporary.path() / "assets" / "unique.bin") << "unique";
  std::ofstream(temporary.path() / "assets" / "preview.png") << "image";
  std::ofstream(temporary.path() / "assets" / "mesh.fbx") << "model";
  std::ofstream(temporary.path() / "assets" / "clip.mp4") << "video";
  const auto analysis = atlas::StorageAnalyzer{}.analyze(temporary.path());
  require(analysis.files == 6, "storage analyzer file count is incorrect");
  require(!analysis.emptyFolders.empty(), "storage analyzer missed an empty folder");
  require(analysis.emptyFolderCount == 1, "storage analyzer empty-folder count is incorrect");
  require(analysis.duplicates.size() == 1 && analysis.duplicates.front().paths.size() == 2,
          "storage analyzer missed exact duplicates");
  require(!analysis.largestFiles.empty() && !analysis.largestFolders.empty(),
          "storage analyzer did not rank paths");
  require(analysis.assetTypes.images == 1 && analysis.assetTypes.models == 1 &&
              analysis.assetTypes.videos == 1 && analysis.assetTypes.other == 3,
          "storage analyzer media breakdown is incorrect");
}

void storageAnalysisFindsPbrMaterialsAndFormats() {
  TemporaryDirectory temporary;
  const auto material = temporary.path() / "Oak Floor";
  std::filesystem::create_directories(material);
  std::ofstream(material / "oak_base_color_4k.png") << "base color";
  std::ofstream(material / "oak_normal_gl_4k.png") << "normal";
  std::ofstream(material / "oak_roughness_4k.png") << "roughness";
  std::ofstream(material / "oak_metallic_4k.png") << "metallic";
  std::ofstream(temporary.path() / "chair.FBX") << "model";
  std::ofstream(temporary.path() / "textures.zip") << "archive";
  std::ofstream(temporary.path() / "backup.7z") << "backup";

  const auto analysis = atlas::StorageAnalyzer{}.analyze(temporary.path(), {}, 25, false);
  require(analysis.pbrMaterials.size() == 1, "PBR material grouping is incorrect");
  require(analysis.pbrMaterials.front().textures.size() == 4,
          "PBR material maps were not grouped together");
  require(analysis.pbrMaterials.front().hasCoreMaps,
          "complete PBR material was not recognized");
  require(analysis.fbxFiles.size() == 1, "FBX inventory is incorrect");
  require(analysis.archiveFiles.size() == 2, "archive inventory is incorrect");
  const auto png = std::find_if(analysis.fileTypes.begin(), analysis.fileTypes.end(),
      [](const auto& type) { return type.extension == ".png"; });
  require(png != analysis.fileTypes.end() && png->files == 4,
          "extension diagnostics are incorrect");
  require(analysis.assetBytes.images > 0 && analysis.assetBytes.models > 0 &&
              analysis.assetBytes.archives > 0,
          "category byte totals are incorrect");
}

void pbrNamesGroupCollectionFoldersConservatively() {
  const auto barkDiffuse = atlas::identifyPbrTexture("bark4x1_01_diffuse2.jpg");
  const auto barkGloss = atlas::identifyPbrTexture("bark4x1_01_glossiness.jpg");
  const auto barkNormal = atlas::identifyPbrTexture("bark4x1_01_normal.jpg");
  const auto barkSpecular = atlas::identifyPbrTexture("bark4x1_01_specular.jpg");
  require(barkDiffuse && barkGloss && barkNormal && barkSpecular,
          "legacy PBR maps were not recognized");
  require(barkDiffuse->canonicalMaterialKey == "bark4x1_01" &&
              barkGloss->canonicalMaterialKey == barkDiffuse->canonicalMaterialKey &&
              barkNormal->canonicalMaterialKey == barkDiffuse->canonicalMaterialKey &&
              barkSpecular->canonicalMaterialKey == barkDiffuse->canonicalMaterialKey,
          "maps in a collection folder were not grouped by filename identity");
  const std::set<atlas::PbrMapKind> barkMaps{
      barkDiffuse->kind, barkGloss->kind, barkNormal->kind, barkSpecular->kind};
  require(atlas::classifyPbrWorkflow(barkMaps) == atlas::PbrWorkflow::SpecularGlossiness,
          "legacy specular/glossiness material was not considered complete");

  const auto broccoliNormal = atlas::identifyPbrTexture(
      "brocolis_24k_DefaultMaterial_Normal.jpg");
  const auto broccoliHeight = atlas::identifyPbrTexture("brocolis_24k_n_height.png");
  require(broccoliNormal && broccoliHeight &&
              broccoliNormal->canonicalMaterialKey == "brocolis" &&
              broccoliHeight->canonicalMaterialKey == "brocolis",
          "resolution and generic exporter tokens split one material identity");

  const auto leafVariant = atlas::identifyPbrTexture("leaf1_diffuse4.jpg");
  const auto leafNormal = atlas::identifyPbrTexture("leaf1_normal3.jpg");
  require(leafVariant && leafNormal && leafVariant->canonicalMaterialKey == "leaf1" &&
              leafNormal->canonicalMaterialKey == "leaf1",
          "numbered map variants split one material identity");
  require(leafVariant->mapVariant == "4" && leafNormal->mapVariant == "3",
          "numbered map variants were not retained for evidence-based sub-grouping");
  const auto cauliflower = atlas::identifyPbrTexture("cauliflower_normal.jpg");
  const auto cauliflowerLeaf = atlas::identifyPbrTexture("cauliflower_leaf_albedo2.jpg");
  require(cauliflower && cauliflowerLeaf &&
              cauliflower->canonicalMaterialKey != cauliflowerLeaf->canonicalMaterialKey,
          "distinct material parts were merged too aggressively");
  const auto generic = atlas::identifyPbrTexture("DefaultMaterial_thickness.jpg");
  require(generic && generic->canonicalMaterialKey.empty(),
          "generic orphan map was incorrectly assigned a material identity");
}

}  // namespace

int main() {
  try {
    catalogPreservesIdentity();
    catalogSearchesAndMaintainsState();
    scannerReportsContents();
    scannerHonorsCancellation();
    scannerHonorsDepth();
    persistentScanCanStopAndResume();
    fileOperationsAreConflictAware();
    jobQueueRunsAndCancelsWork();
    jobQueueShutdownCancelsRunningWork();
    assetKindsAreRecognized();
    storageAnalysisFindsWaste();
    storageAnalysisFindsPbrMaterialsAndFormats();
    pbrNamesGroupCollectionFoldersConservatively();
    std::cout << "All Atlas foundation tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
