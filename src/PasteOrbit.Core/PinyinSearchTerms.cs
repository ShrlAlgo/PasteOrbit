using System.Text;

using TinyPinyin;

namespace PasteOrbit.Core;

/// <summary>
/// 为中文检索文本生成无声调全拼和拼音首字母索引。
/// 索引只保存在内存中，不改变数据库结构或持久化内容。
/// </summary>
internal sealed record PinyinSearchTerms(string FullPinyin, string Initials)
{
    // 剪贴板可能包含很长的文本，限制转换长度可以避免首次搜索时产生明显停顿。
    private const int MaximumIndexedCharacters = 4096;

    public static PinyinSearchTerms? Create(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return null;
        }

        var length = Math.Min(text.Length, MaximumIndexedCharacters);
        var fullPinyin = new StringBuilder(length * 2);
        var initials = new StringBuilder(length);
        var hasChinese = false;

        // 同时构建全拼和首字母，支持两种搜索习惯。
        for (var index = 0; index < length; index++)
        {
            var character = text[index];
            if (PinyinHelper.IsChinese(character))
            {
                var pinyin = PinyinHelper.GetPinyin(character);
                if (!string.IsNullOrEmpty(pinyin))
                {
                    hasChinese = true;
                    fullPinyin.Append(pinyin);
                    initials.Append(pinyin[0]);
                }

                continue;
            }

            // 保留中英混合内容中的字母和数字，并忽略空白及标点，支持连续输入检索。
            if (char.IsLetterOrDigit(character))
            {
                fullPinyin.Append(character);
                initials.Append(character);
            }
        }

        return hasChinese
            ? new PinyinSearchTerms(fullPinyin.ToString(), initials.ToString())
            : null;
    }
}
