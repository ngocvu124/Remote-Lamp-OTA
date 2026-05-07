// ======= Trang files.html =======
window.addEventListener('DOMContentLoaded', () => {
    if (!document.getElementById('fileList')) return; // Chỉ chạy trên files.html
    let currentDir = "/";

    function updateBreadcrumb() {
        const bc = document.getElementById('breadcrumb');
        let parts = currentDir.split('/').filter(p => p);
        let html = `<span onclick="navTo('/')">🏠 SD ROOT</span>`;
        let path = "";
        parts.forEach(p => {
            path += "/" + p;
            html += ` <span style="color: #64748b;">/</span> <span onclick="navTo('${path}')">${p}</span>`;
        });
        bc.innerHTML = html;
    }

    function navTo(path) {
        currentDir = path;
        loadFiles();
    }

    function goUp() {
        if (currentDir === "/") return;
        let parts = currentDir.split('/').filter(p => p);
        parts.pop();
        currentDir = parts.length ? "/" + parts.join("/") : "/";
        loadFiles();
    }

    function formatSize(bytes) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    function loadFiles(retry = 0) {
        setStatus('Đang quét...', '#f59e0b', 'rgba(245, 158, 11, 0.1)');
        fetch('/list?dir=' + encodeURIComponent(currentDir))
        .then(async r => {
            if (!r.ok) throw new Error(await r.text() || 'HTTP ' + r.status);
            return r.json();
        })
        .then(data => {
            data.sort((a, b) => (a.isDir === b.isDir) ? a.name.localeCompare(b.name) : (a.isDir ? -1 : 1));
            const list = document.getElementById('fileList');
            list.innerHTML = data.length === 0 ? '<div style="grid-column: 1/-1; text-align: center; color: #64748b; padding: 50px; font-size: 1.2rem;">Thư mục trống</div>' : '';
            data.forEach(item => {
                const isDir = item.isDir;
                const path = currentDir === "/" ? "/" + item.name : currentDir + "/" + item.name;
                const downloadBtn = !isDir ? `<a href="/download_file?filename=${encodeURIComponent(path)}" download class="icon-btn dl" title="Tải về">📥</a>` : '';
                list.innerHTML += `
                    <div class="file-card">
                        <div class="file-info" ${isDir ? `onclick="navTo('${path}')"` : ''}>
                            <div class="file-icon">${isDir ? '📁' : '📄'}</div>
                            <div class="file-details">
                                <div class="file-name" title="${item.name}" ${isDir ? 'style="color: #38bdf8;"' : ''}>${item.name}</div>
                                <div class="file-size">${isDir ? 'Thư mục' : formatSize(item.size)}</div>
                            </div>
                        </div>
                        <div class="file-actions">
                            ${downloadBtn}
                            <button class="icon-btn" onclick="deleteItem('${path}', ${isDir})" title="Xóa">🗑️</button>
                        </div>
                    </div>`;
            });
            updateBreadcrumb();
            setStatus('Sẵn sàng', '#10b981', 'rgba(16, 185, 129, 0.1)');
        })
        .catch(e => {
            if (retry < 2) setTimeout(() => loadFiles(retry + 1), 300);
            else setStatus('Lỗi: ' + e.message, '#ef4444', 'rgba(239, 68, 68, 0.1)');
        });
    }

    window.goUp = goUp;
    window.navTo = navTo;
    window.deleteItem = function(path, isDir) {
        if (!confirm(isDir ? 'Xóa toàn bộ thư mục?' : 'Xóa tệp này?')) return;
        const formData = new URLSearchParams(); formData.append('path', path);
        fetch('/delete', { method: 'POST', body: formData }).then(r => r.ok ? loadFiles() : alert('Lỗi xóa'));
    };
    window.createFolder = function() {
        const name = prompt("Tên thư mục mới:");
        if (!name) return;
        const path = currentDir === "/" ? "/" + name : currentDir + "/" + name;
        const formData = new URLSearchParams(); formData.append('path', path);
        fetch('/mkdir', { method: 'POST', body: formData }).then(r => r.ok ? loadFiles() : alert('Lỗi tạo thư mục'));
    };
    window.uploadFile = function() {
        const file = document.getElementById('fileInput').files[0];
        if (!file) return;
        setStatus('Đang tải lên...', '#f59e0b', 'rgba(245, 158, 11, 0.1)');
        const formData = new FormData(); formData.append('file', file, file.name);
        fetch('/upload_file?dir=' + encodeURIComponent(currentDir), { method: 'POST', body: formData }).then(r => {
            document.getElementById('fileInput').value = '';
            r.ok ? loadFiles() : setStatus('Lỗi tải lên!', '#ef4444', 'rgba(239, 68, 68, 0.1)');
        });
    };
    loadFiles();
});
// ======= Trang home.html =======
window.addEventListener('DOMContentLoaded', () => {
    const fileInput = document.getElementById('fileInput'), canvas = document.getElementById('canvas'), ctx = canvas?.getContext?.('2d'),
        binPreview = document.getElementById('binPreview'), btx = binPreview?.getContext?.('2d'), 
        uploadBtn = document.getElementById('uploadBtn'), dlLocalBtn = document.getElementById('dlLocalBtn');
    if (!fileInput || !canvas || !ctx || !binPreview || !btx || !uploadBtn || !dlLocalBtn) return;
    let rgb565Data = null;

    function getFileName() {
        let fName = document.getElementById('fileNameInput').value.trim();
        if(!fName) fName = 'bg';
        fName = fName.replace(/\.bin$/i, '');
        return fName + '.bin';
    }

    fileInput.addEventListener('change', function(e) {
        const file = e.target.files[0]; if (!file) return;
        const reader = new FileReader();
        reader.onload = function(event) {
            const img = new Image();
            img.onload = function() {
                const size = Math.min(img.width, img.height), sx = (img.width - size) / 2, sy = (img.height - size) / 2;
                ctx.clearRect(0, 0, 240, 240);
                ctx.drawImage(img, sx, sy, size, size, 0, 0, 240, 240);
                const imgData = ctx.getImageData(0, 0, 240, 240).data;
                rgb565Data = new Uint8Array(240 * 240 * 2);
                let j = 0;
                for (let i = 0; i < imgData.length; i += 4) {
                    const r = imgData[i] >> 3, g = imgData[i+1] >> 2, b = imgData[i+2] >> 3;
                    const rgb565 = (r << 11) | (g << 5) | b;
                    rgb565Data[j++] = rgb565 & 0xFF;        
                    rgb565Data[j++] = (rgb565 >> 8) & 0xFF; 
                }
                renderPreview();
                uploadBtn.disabled = dlLocalBtn.disabled = false;
                setStatus('Đã xử lý ảnh thành công!', '#10b981', 'rgba(16, 185, 129, 0.1)');
            };
            img.src = event.target.result;
        };
        reader.readAsDataURL(file);
    });

    function renderPreview() {
        const imgData = btx.createImageData(240, 240);
        for (let i = 0; i < 240 * 240; i++) {
            const low = rgb565Data[i * 2], high = rgb565Data[i * 2 + 1];
            const val = (high << 8) | low;
            const r = (val >> 11) & 0x1F, g = (val >> 5) & 0x3F, b = val & 0x1F;
            imgData.data[i*4] = (r << 3) | (r >> 2);
            imgData.data[i*4+1] = (g << 2) | (g >> 4);
            imgData.data[i*4+2] = (b << 3) | (b >> 2);
            imgData.data[i*4+3] = 255;
        }
        btx.putImageData(imgData, 0, 0);
    }

    dlLocalBtn.onclick = () => {
        const blob = new Blob([rgb565Data], { type: 'application/octet-stream' });
        const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = getFileName(); a.click();
    };

    uploadBtn.onclick = () => {
        uploadBtn.disabled = true; 
        let finalName = getFileName();
        setStatus('Đang tải ' + finalName + ' lên...', '#f59e0b', 'rgba(245, 158, 11, 0.1)');
        const formData = new FormData(); 
        formData.append('bg', new Blob([rgb565Data], { type: 'application/octet-stream' }), finalName);
        fetch('/upload', { method: 'POST', body: formData }).then(res => res.ok ? location.reload() : alert('Lỗi tải lên!'));
    };
});
// ======= Shared JS for Remote Lamp Web =======
function goHome() { window.location.href = "/"; }
function exitServer() {
    if(confirm("Ngắt kết nối?")) {
        fetch('/exit').then(() => {
            document.body.innerHTML = '<div style="text-align:center; margin-top:30vh; color:#10b981;"><h1>✅ Đã ngắt kết nối</h1></div>';
        });
    }
}
function setStatus(msg, color, bg) {
    const el = document.getElementById('status');
    if (!el) return;
    el.innerText = msg;
    el.style.color = color || '#38bdf8';
    el.style.backgroundColor = bg || 'rgba(56, 189, 248, 0.1)';
    el.style.borderColor = color || 'rgba(56, 189, 248, 0.3)';
}
