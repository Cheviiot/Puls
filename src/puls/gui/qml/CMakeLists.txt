# The QML module of the dashboard: views, components, theme, icons and fonts.
# The puls_gui library links it; the module has no plugin of its own.
set_source_files_properties(Icons.qml Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_library(puls_gui_qml STATIC)
# The type registration of the module reads the metatypes that moc produces.
set_target_properties(puls_gui_qml PROPERTIES AUTOMOC ON)
qt_add_qml_module(puls_gui_qml
    URI Puls
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    QML_FILES
        ActionButton.qml
        AppLogo.qml
        Banner.qml
        Card.qml
        ChoiceList.qml
        DetailRow.qml
        FocusRing.qml
        HeroCard.qml
        Icon.qml
        IconButton.qml
        Icons.qml
        InputField.qml
        Label.qml
        Main.qml
        MetricTile.qml
        PhaseTrack.qml
        ResizeFrame.qml
        ResultRow.qml
        SegmentedControl.qml
        SettingsPanel.qml
        Spinner.qml
        Stepper.qml
        SwitchRow.qml
        Theme.qml
        ThinScrollBar.qml
        TitleBar.qml
        Toast.qml
        ToolTip.qml
        ValueSlider.qml
    RESOURCES
        fonts/Inter-Medium.ttf
        fonts/Inter-Regular.ttf
        fonts/Inter-SemiBold.ttf
        fonts/InterDisplay-SemiBold.ttf)
