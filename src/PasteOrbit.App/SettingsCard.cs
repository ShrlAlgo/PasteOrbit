using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace PasteOrbit.App;

/// <summary>
/// 设置页使用的标题、描述和右侧内容布局容器。
/// </summary>
public sealed class SettingsCard : ContentControl
{
    public static readonly DependencyProperty HeaderProperty = DependencyProperty.Register(
        nameof(Header),
        typeof(string),
        typeof(SettingsCard),
        new PropertyMetadata(string.Empty));

    public static readonly DependencyProperty DescriptionProperty = DependencyProperty.Register(
        nameof(Description),
        typeof(string),
        typeof(SettingsCard),
        new PropertyMetadata(string.Empty));

    public static readonly DependencyProperty ContentAlignmentProperty = DependencyProperty.Register(
        nameof(ContentAlignment),
        typeof(HorizontalAlignment),
        typeof(SettingsCard),
        new PropertyMetadata(HorizontalAlignment.Right));

    public string Header
    {
        get => (string)GetValue(HeaderProperty);
        set => SetValue(HeaderProperty, value);
    }

    public string Description
    {
        get => (string)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public HorizontalAlignment ContentAlignment
    {
        get => (HorizontalAlignment)GetValue(ContentAlignmentProperty);
        set => SetValue(ContentAlignmentProperty, value);
    }
}
