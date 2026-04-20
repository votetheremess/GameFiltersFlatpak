data_dir = get_option('datadir')
vulkan_layer_dir = join_paths(data_dir, 'vulkan', 'implicit_layer.d')

configure_file(
    input: 'game-filters-flatpak.json.in',
    output: 'game-filters-flatpak.json',
    copy: true,
    install: true,
    install_dir: vulkan_layer_dir,
)

install_data(
    'game-filters-flatpak.conf',
    install_dir: join_paths(data_dir, 'game-filters-flatpak'),
)
