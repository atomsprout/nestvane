/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  docs: [
    'README',
    {
      type: 'category',
      label: 'How it works',
      collapsed: false,
      items: ['architecture', 'static-rewriting', 'kernel-layer', 'devices', 'design-notes'],
    },
    {
      type: 'category',
      label: 'Using and building',
      collapsed: false,
      items: ['installing', 'building'],
    },
    {
      type: 'category',
      label: 'The project',
      collapsed: false,
      items: ['status', 'measurements', 'comparison'],
    },
  ],
};

export default sidebars;
