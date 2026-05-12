// ======= Trang files.html =======
const authToken = new URLSearchParams(window.location.search).get('t') || '';

function apiUrl(path) {
    if (!authToken) return path;
    const sep = path.includes('?') ? '&' : '?';
    return path + sep + 't=' + encodeURIComponent(authToken);
}

function keepAuthLinks() {
    if (!authToken) return;
    document.querySelectorAll('a[href^="/"]').forEach(a => {
        const href = a.getAttribute('href');
        if (href && !href.includes('t=')) a.setAttribute('href', apiUrl(href));
    });
}

window.addEventListener('DOMContentLoaded', () => {
    keepAuthLinks();
    if (!document.getElementById('fileList')) return; // Chỉ chạy trên files.html

    // Hiển thị dung lượng bộ nhớ SD
    function updateStorageBar() {
        const bar = document.getElementById('storageBar');
        const text = document.getElementById('storageText');
        if (!bar || !text) return;
        text.innerText = 'Đang tải...';
        bar.style.width = '0%';
        fetch(apiUrl('/storage_info')).then(async r => {
            if (!r.ok) throw new Error(await r.text() || 'HTTP ' + r.status);
            return r.json();
        }).then(data => {
            const total = data.total || 0, used = data.used || 0, free = data.free || 0;
            const percent = total > 0 ? Math.round(used * 100 / total) : 0;
            bar.style.width = percent + '%';
            text.innerText = `Đã dùng: ${formatSize(used)} / ${formatSize(total)} (${percent}%) | Còn trống: ${formatSize(free)}`;
            bar.style.background = percent > 90 ? '#ef4444' : (percent > 70 ? '#f59e0b' : '#38bdf8');
        }).catch(e => {
            text.innerText = 'Không đọc được thẻ nhớ!';
            bar.style.width = '0%';
            bar.style.background = '#ef4444';
        });
    }
    updateStorageBar();
    setInterval(updateStorageBar, 10000);
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
        fetch(apiUrl('/list?dir=' + encodeURIComponent(currentDir)))
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
                const downloadBtn = !isDir ? `<a href="${apiUrl('/download_file?filename=' + encodeURIComponent(path))}" download class="icon-btn dl" title="Tải về">📥</a>` : '';
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
        fetch(apiUrl('/delete'), { method: 'POST', body: formData }).then(r => r.ok ? loadFiles() : alert('Lỗi xóa'));
    };
    window.createFolder = function() {
        const name = prompt("Tên thư mục mới:");
        if (!name) return;
        const path = currentDir === "/" ? "/" + name : currentDir + "/" + name;
        const formData = new URLSearchParams(); formData.append('path', path);
        fetch(apiUrl('/mkdir'), { method: 'POST', body: formData }).then(r => r.ok ? loadFiles() : alert('Lỗi tạo thư mục'));
    };
    window.uploadFile = function() {
        const file = document.getElementById('fileInput').files[0];
        if (!file) return;
        setStatus('Đang tải lên...', '#f59e0b', 'rgba(245, 158, 11, 0.1)');
        const formData = new FormData(); formData.append('file', file, file.name);
        fetch(apiUrl('/upload_file?dir=' + encodeURIComponent(currentDir)), { method: 'POST', body: formData }).then(r => {
            document.getElementById('fileInput').value = '';
            r.ok ? loadFiles() : setStatus('Lỗi tải lên!', '#ef4444', 'rgba(239, 68, 68, 0.1)');
        });
    };
    loadFiles();
});
// ======= Trang home.html =======
window.addEventListener('DOMContentLoaded', () => {
    keepAuthLinks();
    const fileInput = document.getElementById('fileInput'), canvas = document.getElementById('canvas'), ctx = canvas?.getContext?.('2d'),
        binPreview = document.getElementById('binPreview'), btx = binPreview?.getContext?.('2d'), 
        uploadBtn = document.getElementById('uploadBtn'), dlLocalBtn = document.getElementById('dlLocalBtn'),
        refreshBtn = document.getElementById('refreshBtn'),
        errorMsg = document.getElementById('errorMsg'),
        binSizeInfo = document.getElementById('binSizeInfo'),
        originalImg = document.getElementById('originalImg');
    // Text ký
    const signText = document.getElementById('signText'), signColor = document.getElementById('signColor');
    let sign = { text: '', color: '#38bdf8', x: 120, y: 220, dragging: false, offsetX: 0, offsetY: 0 };
    // Cropper popup
    const cropModal = document.getElementById('cropModal'), cropperImg = document.getElementById('cropperImg'), cropOkBtn = document.getElementById('cropOkBtn'), cropCancelBtn = document.getElementById('cropCancelBtn');
    let cropper = null, cropSrc = null;
    if (!fileInput || !canvas || !ctx || !binPreview || !btx || !uploadBtn || !dlLocalBtn || !refreshBtn || !errorMsg || !binSizeInfo || !originalImg || !cropModal || !cropperImg || !cropOkBtn || !cropCancelBtn) return;
    let rgb565Data = null;

    function getFileName() {
        let fName = document.getElementById('fileNameInput').value.trim();
        if(!fName) fName = 'bg';
        fName = fName.replace(/\.bin$/i, '');
        return fName + '.bin';
    }

    function resetForm() {
            sign.text = '';
            sign.x = 120;
            sign.y = 220;
            sign.color = '#38bdf8';
            if(signText) signText.value = '';
            if(signColor) signColor.value = '#38bdf8';
            // Xử lý nhập text/chữ ký
            if(signText) signText.oninput = function() {
                sign.text = this.value;
                redrawCanvasWithSign();
            };
            if(signColor) signColor.oninput = function() {
                sign.color = this.value;
                redrawCanvasWithSign();
            };

            // Kéo/thả chữ ký trên canvas
            let dragging = false;
            if(canvas) {
                canvas.addEventListener('mousedown', function(e) {
                    const rect = canvas.getBoundingClientRect();
                    const mx = e.clientX - rect.left, my = e.clientY - rect.top;
                    ctx.font = 'bold 22px sans-serif';
                    const w = ctx.measureText(sign.text).width, h = 24;
                    if(sign.text && mx > sign.x-w/2-8 && mx < sign.x+w/2+8 && my > sign.y-h && my < sign.y+8) {
                        dragging = true;
                        sign.dragging = true;
                        sign.offsetX = mx - sign.x;
                        sign.offsetY = my - sign.y;
                    }
                });
                canvas.addEventListener('mousemove', function(e) {
                    if(!dragging) return;
                    const rect = canvas.getBoundingClientRect();
                    sign.x = e.clientX - rect.left - sign.offsetX;
                    sign.y = e.clientY - rect.top - sign.offsetY;
                    redrawCanvasWithSign();
                });
                canvas.addEventListener('mouseup', function() { dragging = false; sign.dragging = false; });
                canvas.addEventListener('mouseleave', function() { dragging = false; sign.dragging = false; });
            }
        fileInput.value = "";
        document.getElementById('fileNameInput').value = "bg1";
        ctx.clearRect(0, 0, 240, 240);
        btx.clearRect(0, 0, 240, 240);
        rgb565Data = null;
        uploadBtn.disabled = dlLocalBtn.disabled = true;
        errorMsg.style.display = 'none';
        binSizeInfo.style.display = 'none';
        originalImg.style.display = 'none';
        setStatus('Sẵn sàng', '#10b981', 'rgba(16, 185, 129, 0.1)');
    }
    refreshBtn.onclick = resetForm;

    fileInput.addEventListener('change', function(e) {
        errorMsg.style.display = 'none';
        binSizeInfo.style.display = 'none';
        originalImg.style.display = 'none';
        const file = e.target.files[0]; 
        if (!file) return;
        
        if (!file.type.match(/^image\/(jpeg|png)$/)) {
            errorMsg.innerText = 'Chỉ hỗ trợ ảnh JPG hoặc PNG!';
            errorMsg.style.display = 'block';
            return;
        }
        if (file.size > 20*1024*1024) {
            errorMsg.innerText = 'Ảnh quá lớn!';
            errorMsg.style.display = 'block';
            return;
        }

        const reader = new FileReader();
        reader.onload = function(event) {
            cropSrc = event.target.result;
            
            // 1. Gán source cho ảnh
            cropperImg.src = cropSrc;
            
            // 2. Mở popup lên trước để thẻ img có kích thước thực tế
            cropModal.style.display = 'flex';
            
            // 3. Xóa cái hướng dẫn cũ đi cho gọn
            const guide = document.getElementById('cropperGuide');
            if (guide) guide.innerHTML = '';

            // 4. CHỜ ẢNH LOAD XONG TRÊN DOM MỚI GỌI CROPPER
            cropperImg.onload = function() {
                if (cropper) { 
                    cropper.destroy(); 
                    cropper = null; 
                }
                cropper = new Cropper(cropperImg, {
                    aspectRatio: 1,
                    viewMode: 1,
                    autoCropArea: 1,
                    background: false,
                    movable: true,
                    zoomable: true,
                    scalable: false,
                    rotatable: false,
                    responsive: true,
                    minCropBoxWidth: 240,
                    minCropBoxHeight: 240,
                    cropBoxResizable: true,
                    cropBoxMovable: true,
                    ready() {
                        // Căn giữa khung crop 240x240
                        const imgData = cropper.getImageData();
                        const left = (imgData.naturalWidth - 240) / 2;
                        const top = (imgData.naturalHeight - 240) / 2;
                        cropper.setCropBoxData({ width: 240, height: 240, left: left, top: top });
                    }
                });
            };
        };
        reader.readAsDataURL(file);
    });

    cropOkBtn.onclick = function() {
        if (!cropper) return;
        // Lấy vùng crop, vẽ lên canvas
        const cropData = cropper.getData(true);
        const img = new Image();
        img.onload = function() {
            ctx.clearRect(0, 0, 240, 240);
            ctx.drawImage(img, cropData.x, cropData.y, cropData.width, cropData.height, 0, 0, 240, 240);
            redrawCanvasWithSign();
            // Hiện preview ảnh gốc đã crop
            originalImg.src = canvas.toDataURL('image/png');
            originalImg.style.display = 'block';
            // Chuyển sang RGB565
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
            binSizeInfo.innerText = `Kích thước file .bin: ${(rgb565Data.length/1024).toFixed(1)} KB`;
            binSizeInfo.style.display = 'block';
            cropModal.style.display = 'none';
            cropper.destroy(); cropper = null;
        };
        img.src = cropSrc;
        // Vẽ lại canvas với chữ ký
        function redrawCanvasWithSign() {
            // Lấy ảnh gốc trên canvas
            const imgData = ctx.getImageData(0, 0, 240, 240);
            ctx.clearRect(0, 0, 240, 240);
            ctx.putImageData(imgData, 0, 0);
            if(sign.text) {
                ctx.save();
                ctx.font = 'bold 22px sans-serif';
                ctx.textAlign = 'center';
                ctx.textBaseline = 'bottom';
                ctx.strokeStyle = '#000a';
                ctx.lineWidth = 4;
                ctx.strokeText(sign.text, sign.x, sign.y);
                ctx.fillStyle = sign.color;
                ctx.fillText(sign.text, sign.x, sign.y);
                ctx.restore();
            }
        }
    };
    cropCancelBtn.onclick = function() {
        cropModal.style.display = 'none';
        if (cropper) { 
            cropper.destroy(); 
            cropper = null; 
        }
        cropperImg.src = ""; // Thêm dòng này để dọn dẹp
        fileInput.value = "";
    };

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

    dlLocalBtn.title = "Tải file .bin về máy tính";
    uploadBtn.title = "Tải file .bin lên ESP32";

    dlLocalBtn.onclick = () => {
        if (!rgb565Data) return;
        const blob = new Blob([rgb565Data], { type: 'application/octet-stream' });
        const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = getFileName(); a.click();
    };

    uploadBtn.onclick = () => {
        if (!rgb565Data) return;
        uploadBtn.disabled = true;
        let finalName = getFileName();
        setStatus('Đang tải ' + finalName + ' lên...', '#f59e0b', 'rgba(245, 158, 11, 0.1)');
        // Hiện thanh tiến trình
        const progressWrap = document.getElementById('uploadProgressWrap');
        const progressBar = document.getElementById('uploadProgressBar');
        const progressText = document.getElementById('uploadProgressText');
        if(progressWrap) progressWrap.style.display = 'block';
        if(progressBar) progressBar.style.width = '0%';
        if(progressText) progressText.innerText = '0%';

        const formData = new FormData();
        formData.append('bg', new Blob([rgb565Data], { type: 'application/octet-stream' }), finalName);
        // Dùng XMLHttpRequest để theo dõi tiến trình
        const xhr = new XMLHttpRequest();
        xhr.open('POST', apiUrl('/upload'), true);
        xhr.upload.onprogress = function(e) {
            if (e.lengthComputable) {
                const percent = Math.round(e.loaded * 100 / e.total);
                if(progressBar) progressBar.style.width = percent + '%';
                if(progressText) progressText.innerText = percent + '%';
            }
        };
        xhr.onload = function() {
            if(progressBar) progressBar.style.width = '100%';
            if(progressText) progressText.innerText = '100%';
            setTimeout(() => { if(progressWrap) progressWrap.style.display = 'none'; }, 600);
            if (xhr.status === 200) {
                location.reload();
            } else {
                alert('Lỗi tải lên!');
                uploadBtn.disabled = false;
            }
        };
        xhr.onerror = function() {
            if(progressWrap) progressWrap.style.display = 'none';
            alert('Lỗi kết nối khi upload!');
            uploadBtn.disabled = false;
        };
        xhr.send(formData);
    };
});
// ======= Shared JS for Remote Lamp Web =======
function goHome() { window.location.href = apiUrl("/"); }
function exitServer() {
    if(confirm("Ngắt kết nối?")) {
        fetch(apiUrl('/exit')).then(() => {
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
