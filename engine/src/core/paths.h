// paths.h — 実行ファイルの場所から決まるフォルダ(シェーダー・ログ・トレースの既定の置き場所)。Windows 専用。
// PC 固有のパスを埋め込まないため、既定の置き場所はいつも exe の横にする(ADR-0006)。
#pragma once

#include <filesystem>

#include "core/aliases.h"

namespace bicameral {

    // 実行中の exe のあるフォルダ(長いパスでも切り詰めない)。取れなければ空のパス(= 今のフォルダからの相対)
    fs::path ExecutableDirectory();

}  // namespace bicameral
