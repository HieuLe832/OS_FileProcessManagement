# Các phiên bản báo cáo

Báo cáo trên nhánh `latex` đã được rút gọn. Các bản dài hơn vẫn còn trong lịch sử git:

| Commit | Số trang | Nội dung |
|---|---|---|
| `655128c` | 16 | Bản rút gọn, mục 3 chỉ có chữ. Bản hiện tại (18 trang) thêm vào mục 3 các đoạn trích ngắn từ mã nguồn xv6. |
| `aba5e79` | 32 | Bản dài, ngay trước khi rút gọn. Đã bỏ code khỏi báo cáo và sửa lỗi trình bày. PDF có sẵn trong commit. |
| `bdcdda8` | 37 | Bản dài nhất, mục 3 và 4 còn nguyên các đoạn code. PDF không có trong commit, phải compile lại. |

## Lấy lại bản cũ

Lấy PDF của bản 32 trang:

```
git show aba5e79:24120048_24120090_24120405_Report/main.pdf > bao-cao-32-trang.pdf
```

Xem nội dung cũ của một mục, ví dụ mục 4.2:

```
git show aba5e79:24120048_24120090_24120405_Report/sections/42-files.tex
```

Lấy cả bản cũ ra một thư mục riêng để compile (không ảnh hưởng nhánh hiện tại):

```
git worktree add ../bao-cao-dai bdcdda8
cd ../bao-cao-dai/24120048_24120090_24120405_Report
docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v "$PWD":/doc -w /doc \
  texlive/texlive:latest latexmk -pdf -shell-escape main.tex
```

Xong thì xóa thư mục đó bằng `git worktree remove --force ../bao-cao-dai`.
