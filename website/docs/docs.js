document.addEventListener('DOMContentLoaded', () => {
  'use strict';

  // Mobile sidebar toggle
  const toggle = document.querySelector('.docs-sidebar-toggle');
  const sidebar = document.querySelector('.docs-sidebar');
  if (toggle && sidebar) {
    toggle.addEventListener('click', () => {
      const isOpen = sidebar.classList.toggle('open');
      toggle.setAttribute('aria-expanded', String(isOpen));
    });
    // Close sidebar when a link is clicked (mobile)
    sidebar.querySelectorAll('.sidebar-link').forEach((link) => {
      link.addEventListener('click', () => {
        sidebar.classList.remove('open');
      });
    });
  }

  // Highlight active page in sidebar based on current filename
  const currentPage = window.location.pathname.split('/').pop() || 'index.html';
  document.querySelectorAll('.sidebar-link').forEach((link) => {
    const href = link.getAttribute('href');
    if (href === currentPage || (currentPage === '' && href === 'index.html')) {
      link.classList.add('active');
    }
  });

  // Smooth scroll for anchor links within the same page
  document.querySelectorAll('a[href^="#"]').forEach((anchor) => {
    anchor.addEventListener('click', (e) => {
      const target = document.querySelector(anchor.getAttribute('href'));
      if (target) {
        e.preventDefault();
        target.scrollIntoView({ behavior: 'smooth' });
      }
    });
  });

  // Active section highlighting on scroll (for pages with anchor sections)
  const headings = document.querySelectorAll('.docs-content h2[id], .docs-content h3[id]');
  if (headings.length > 0) {
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) {
            const id = entry.target.getAttribute('id');
            document.querySelectorAll('.sidebar-link').forEach((link) => {
              link.classList.remove('active-section');
              if (link.getAttribute('href') === '#' + id) {
                link.classList.add('active-section');
              }
            });
          }
        });
      },
      { rootMargin: '-80px 0px -60% 0px', threshold: 0 }
    );
    headings.forEach((h) => observer.observe(h));
  }
});
