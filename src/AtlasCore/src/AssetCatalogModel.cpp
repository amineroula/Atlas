#include <AtlasCore/AssetCatalogModel.h>

#include <algorithm>
#include <utility>

namespace atlas {

void AssetCatalogModel::upsert(CatalogAsset asset) {
  const auto sameIdentity = [&asset](const CatalogAsset& existing) {
    if (!asset.atlasId.empty() && existing.atlasId == asset.atlasId) return true;
    return !asset.source.provider.empty() && !asset.source.vendorId.empty() &&
           existing.source.provider == asset.source.provider &&
           existing.source.vendorId == asset.source.vendorId;
  };

  const auto it = std::find_if(assets_.begin(), assets_.end(), sameIdentity);
  if (it == assets_.end()) {
    assets_.push_back(std::move(asset));
  } else {
    *it = std::move(asset);
  }
}

const std::vector<CatalogAsset>& AssetCatalogModel::assets() const noexcept {
  return assets_;
}

std::vector<CatalogAsset> AssetCatalogModel::byCategory(const std::string& category) const {
  std::vector<CatalogAsset> result;
  std::copy_if(assets_.begin(), assets_.end(), std::back_inserter(result),
               [&category](const CatalogAsset& asset) { return asset.category == category; });
  return result;
}

std::vector<CatalogAsset> AssetCatalogModel::byProvider(const std::string& provider) const {
  std::vector<CatalogAsset> result;
  std::copy_if(assets_.begin(), assets_.end(), std::back_inserter(result),
               [&provider](const CatalogAsset& asset) { return asset.source.provider == provider; });
  return result;
}

std::vector<CatalogAsset> AssetCatalogModel::byAvailability(AssetAvailability availability) const {
  std::vector<CatalogAsset> result;
  std::copy_if(assets_.begin(), assets_.end(), std::back_inserter(result),
               [availability](const CatalogAsset& asset) { return asset.availability == availability; });
  return result;
}

}  // namespace atlas
