document.addEventListener('DOMContentLoaded', () => {
  'use strict';

  // ── Scroll-triggered fade-in ──────────────────
  const animatedEls = document.querySelectorAll('.feature-card, .why-card, .why-card-small');
  if (animatedEls.length > 0) {
    const observer = new IntersectionObserver((entries) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          const parent = entry.target.parentElement;
          if (parent) {
            const siblings = parent.querySelectorAll('.feature-card, .why-card, .why-card-small');
            const index = Array.prototype.indexOf.call(siblings, entry.target);
            if (index > 0) {
              entry.target.style.transitionDelay = (index * 0.12) + 's';
            }
          }
          entry.target.classList.add('visible');
          observer.unobserve(entry.target);
        }
      });
    }, { threshold: 0.15 });
    animatedEls.forEach((el) => observer.observe(el));
  }

  // ── Mobile nav toggle ─────────────────────────
  const navToggle = document.querySelector('.nav-toggle');
  const navLinks = document.querySelector('.nav-links');
  if (navToggle && navLinks) {
    navToggle.addEventListener('click', () => {
      const isOpen = navLinks.classList.toggle('open');
      navToggle.classList.toggle('open');
      navToggle.setAttribute('aria-expanded', String(isOpen));
    });
    navLinks.querySelectorAll('a').forEach((link) => {
      link.addEventListener('click', () => {
        navLinks.classList.remove('open');
        navToggle.classList.remove('open');
        navToggle.setAttribute('aria-expanded', 'false');
      });
    });
  }

  // ── Nav shadow + active link ──────────────────
  const nav = document.querySelector('.nav');
  const sections = document.querySelectorAll('section[id]');
  const navAnchors = document.querySelectorAll('.nav-links a[href^="#"]');
  function updateNav() {
    if (nav) {
      nav.classList.toggle('nav-scrolled', window.scrollY > 50);
    }
    let currentId = '';
    const scrollY = window.scrollY + 120;
    sections.forEach((section) => {
      const top = section.offsetTop;
      const height = section.offsetHeight;
      if (scrollY >= top && scrollY < top + height) {
        currentId = section.getAttribute('id');
      }
    });
    navAnchors.forEach((a) => {
      a.classList.toggle('active', a.getAttribute('href') === '#' + currentId);
    });
  }
  window.addEventListener('scroll', updateNav, { passive: true });
  updateNav();

  // ── Scroll-to-top ─────────────────────────────
  const scrollTopBtn = document.querySelector('.scroll-top');
  if (scrollTopBtn) {
    window.addEventListener('scroll', () => {
      scrollTopBtn.classList.toggle('visible', window.scrollY > 600);
    }, { passive: true });
    scrollTopBtn.addEventListener('click', () => {
      window.scrollTo({ top: 0, behavior: 'smooth' });
    });
  }

  // ── Terminal typing animation ─────────────────
  const TERMINAL_FRAMES = [
    { type: 'cmd', text: '$ ./rayforce' },
    { type: 'out', lines: [
      '<span class="t-ok">Rayforce 2.0</span> \u2014 Rayfall REPL',
      'Type <span class="t-str">:help</span> for commands',
      '',
    ]},
    { type: 'wait', ms: 800 },
    { type: 'cmd', text: 'rf> (set t (table [Symbol Side Qty]' },
    { type: 'cmd', text: '      (list [AAPL GOOG MSFT AAPL GOOG]' },
    { type: 'cmd', text: '            [Buy Sell Buy Sell Buy]' },
    { type: 'cmd', text: '            [100 200 150 300 250])))' },
    { type: 'wait', ms: 400 },
    { type: 'out', lines: [
      '<span class="t-tbl">Symbol Side Qty</span>',
      '<span class="t-tbl">------ ---- ---</span>',
      'AAPL   Buy  100',
      'GOOG   Sell 200',
      'MSFT   Buy  150',
      'AAPL   Sell 300',
      'GOOG   Buy  250',
      '',
    ]},
    { type: 'wait', ms: 1200 },
    { type: 'cmd', text: 'rf> (select {from:t by: Symbol Qty: (sum Qty)})' },
    { type: 'wait', ms: 400 },
    { type: 'out', lines: [
      '<span class="t-tbl">Symbol Qty</span>',
      '<span class="t-tbl">------ ---</span>',
      'AAPL   400',
      'GOOG   450',
      'MSFT   150',
      '',
    ]},
    { type: 'wait', ms: 1200 },
    { type: 'cmd', text: "rf> (pivot t 'Symbol 'Side 'Qty sum)" },
    { type: 'wait', ms: 400 },
    { type: 'out', lines: [
      '<span class="t-tbl">Symbol Buy Sell</span>',
      '<span class="t-tbl">------ --- ----</span>',
      'AAPL   100 300',
      'GOOG   250 200',
      'MSFT   150    ',
      '',
    ]},
    { type: 'wait', ms: 4000 },
    { type: 'clear' },
  ];

  class TerminalAnimation {
    constructor(el, frames) {
      this.el = el;
      this.frames = frames;
      this.running = false;
    }

    async start() {
      if (this.running) return;
      this.running = true;
      while (this.running) {
        this.el.innerHTML = '';
        for (const frame of this.frames) {
          if (!this.running) return;
          switch (frame.type) {
            case 'cmd': await this.typeCommand(frame.text); break;
            case 'out': await this.showOutput(frame.lines); break;
            case 'wait': await this.wait(frame.ms); break;
            case 'clear':
              await this.wait(300);
              this.el.style.opacity = '0';
              await this.wait(300);
              this.el.innerHTML = '';
              this.el.style.opacity = '1';
              break;
          }
        }
      }
    }

    stop() {
      this.running = false;
    }

    async typeCommand(text) {
      const line = document.createElement('div');
      line.className = 'term-line term-cmd';
      this.el.appendChild(line);

      const cursor = document.createElement('span');
      cursor.className = 'term-cursor';
      cursor.textContent = '\u2588';

      for (let i = 0; i < text.length; i++) {
        if (!this.running) return;
        line.textContent = text.slice(0, i + 1);
        line.appendChild(cursor);
        this.scrollToBottom();
        await this.wait(25 + Math.random() * 35);
      }

      cursor.remove();
      await this.wait(120);
    }

    async showOutput(lines) {
      for (const html of lines) {
        if (!this.running) return;
        const line = document.createElement('div');
        line.className = 'term-line term-out';
        line.innerHTML = html;
        this.el.appendChild(line);
        this.scrollToBottom();
        await this.wait(35);
      }
    }

    scrollToBottom() {
      this.el.scrollTop = this.el.scrollHeight;
    }

    wait(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
    }
  }

  // Start terminal when scrolled into view
  const terminalOutput = document.getElementById('terminal-output');
  if (terminalOutput) {
    const terminal = new TerminalAnimation(terminalOutput, TERMINAL_FRAMES);
    const termObserver = new IntersectionObserver((entries) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          terminal.start();
          termObserver.unobserve(entry.target);
        }
      });
    }, { threshold: 0.3 });
    termObserver.observe(terminalOutput);
  }
});
