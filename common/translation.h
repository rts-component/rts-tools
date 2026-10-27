#ifndef TRANSLATION_H
#define TRANSLATION_H

#include <QString>
#include <QStringList>

// 界面语言：用户的选择保存在数据目录的 settings/language.json，切换后立即生效。
// 翻译文件是构建时编进二进制的资源 :/i18n/<baseName>_<locale>，源语言（英文）没有翻译文件。
namespace UiLanguage {

inline constexpr char kSystem[] = "system";   // 跟随系统界面语言
inline constexpr char kChinese[] = "zh_CN";
inline constexpr char kEnglish[] = "en";      // 源语言，不加载翻译

// 可选的语言标识：system、zh_CN、en
QStringList available();

// 已保存的选择；没设置过或值无效时返回 system
QString saved();

// 保存选择；值无效时按 system 处理
void save(const QString &language);

// 按保存的选择加载并安装翻译，返回是否装上了翻译文件。
// 切换语言时调用即可，Qt 会给顶层窗口发 LanguageChange，界面在 changeEvent 里重译。
bool apply(const QString &baseName);

} // namespace UiLanguage

#endif // TRANSLATION_H
